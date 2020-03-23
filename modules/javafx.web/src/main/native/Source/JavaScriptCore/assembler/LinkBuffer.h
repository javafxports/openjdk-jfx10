/*
 * Copyright (C) 2009-2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(ASSEMBLER)

#define DUMP_LINK_STATISTICS 0
#define DUMP_CODE 0

#define GLOBAL_THUNK_ID reinterpret_cast<void*>(static_cast<intptr_t>(-1))
#define REGEXP_CODE_ID reinterpret_cast<void*>(static_cast<intptr_t>(-2))
#define CSS_CODE_ID reinterpret_cast<void*>(static_cast<intptr_t>(-3))

#include "JITCompilationEffort.h"
#include "MacroAssembler.h"
#include "MacroAssemblerCodeRef.h"
#include <wtf/DataLog.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class CodeBlock;

// LinkBuffer:
//
// This class assists in linking code generated by the macro assembler, once code generation
// has been completed, and the code has been copied to is final location in memory.  At this
// time pointers to labels within the code may be resolved, and relative offsets to external
// addresses may be fixed.
//
// Specifically:
//   * Jump objects may be linked to external targets,
//   * The address of Jump objects may taken, such that it can later be relinked.
//   * The return address of a Call may be acquired.
//   * The address of a Label pointing into the code may be resolved.
//   * The value referenced by a DataLabel may be set.
//
class LinkBuffer {
    WTF_MAKE_NONCOPYABLE(LinkBuffer); WTF_MAKE_FAST_ALLOCATED;

    template<PtrTag tag> using CodePtr = MacroAssemblerCodePtr<tag>;
    template<PtrTag tag> using CodeRef = MacroAssemblerCodeRef<tag>;
    typedef MacroAssembler::Label Label;
    typedef MacroAssembler::Jump Jump;
    typedef MacroAssembler::PatchableJump PatchableJump;
    typedef MacroAssembler::JumpList JumpList;
    typedef MacroAssembler::Call Call;
    typedef MacroAssembler::DataLabelCompact DataLabelCompact;
    typedef MacroAssembler::DataLabel32 DataLabel32;
    typedef MacroAssembler::DataLabelPtr DataLabelPtr;
    typedef MacroAssembler::ConvertibleLoadLabel ConvertibleLoadLabel;
#if ENABLE(BRANCH_COMPACTION)
    typedef MacroAssembler::LinkRecord LinkRecord;
    typedef MacroAssembler::JumpLinkType JumpLinkType;
#endif

public:
    LinkBuffer(MacroAssembler& macroAssembler, void* ownerUID, JITCompilationEffort effort = JITCompilationMustSucceed)
        : m_size(0)
        , m_didAllocate(false)
#ifndef NDEBUG
        , m_completed(false)
#endif
    {
        linkCode(macroAssembler, ownerUID, effort);
    }

    template<PtrTag tag>
    LinkBuffer(MacroAssembler& macroAssembler, MacroAssemblerCodePtr<tag> code, size_t size, JITCompilationEffort effort = JITCompilationMustSucceed, bool shouldPerformBranchCompaction = true)
        : m_size(size)
        , m_didAllocate(false)
#ifndef NDEBUG
        , m_completed(false)
#endif
        , m_code(code.template retagged<LinkBufferPtrTag>())
    {
#if ENABLE(BRANCH_COMPACTION)
        m_shouldPerformBranchCompaction = shouldPerformBranchCompaction;
#else
        UNUSED_PARAM(shouldPerformBranchCompaction);
#endif
        linkCode(macroAssembler, 0, effort);
    }

    ~LinkBuffer()
    {
    }

    bool didFailToAllocate() const
    {
        return !m_didAllocate;
    }

    bool isValid() const
    {
        return !didFailToAllocate();
    }

    // These methods are used to link or set values at code generation time.

    template<PtrTag tag, typename Func, typename = std::enable_if_t<std::is_function<typename std::remove_pointer<Func>::type>::value>>
    void link(Call call, Func funcName)
    {
        FunctionPtr<tag> function(funcName);
        link(call, function);
    }

    template<PtrTag tag>
    void link(Call call, FunctionPtr<tag> function)
    {
        ASSERT(call.isFlagSet(Call::Linkable));
        call.m_label = applyOffset(call.m_label);
        MacroAssembler::linkCall(code(), call, function);
    }

    template<PtrTag tag>
    void link(Call call, CodeLocationLabel<tag> label)
    {
        link(call, FunctionPtr<tag>(label));
    }

    template<PtrTag tag>
    void link(Jump jump, CodeLocationLabel<tag> label)
    {
        jump.m_label = applyOffset(jump.m_label);
        MacroAssembler::linkJump(code(), jump, label);
    }

    template<PtrTag tag>
    void link(const JumpList& list, CodeLocationLabel<tag> label)
    {
        for (const Jump& jump : list.jumps())
            link(jump, label);
    }

    void patch(DataLabelPtr label, void* value)
    {
        AssemblerLabel target = applyOffset(label.m_label);
        MacroAssembler::linkPointer(code(), target, value);
    }

    template<PtrTag tag>
    void patch(DataLabelPtr label, CodeLocationLabel<tag> value)
    {
        AssemblerLabel target = applyOffset(label.m_label);
        MacroAssembler::linkPointer(code(), target, value);
    }

    // These methods are used to obtain handles to allow the code to be relinked / repatched later.

    template<PtrTag tag>
    CodeLocationLabel<tag> entrypoint()
    {
        return CodeLocationLabel<tag>(tagCodePtr<tag>(code()));
    }

    template<PtrTag tag>
    CodeLocationCall<tag> locationOf(Call call)
    {
        ASSERT(call.isFlagSet(Call::Linkable));
        ASSERT(!call.isFlagSet(Call::Near));
        return CodeLocationCall<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(call.m_label)));
    }

    template<PtrTag tag>
    CodeLocationNearCall<tag> locationOfNearCall(Call call)
    {
        ASSERT(call.isFlagSet(Call::Linkable));
        ASSERT(call.isFlagSet(Call::Near));
        return CodeLocationNearCall<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(call.m_label)),
            call.isFlagSet(Call::Tail) ? NearCallMode::Tail : NearCallMode::Regular);
    }

    template<PtrTag tag>
    CodeLocationLabel<tag> locationOf(PatchableJump jump)
    {
        return CodeLocationLabel<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(jump.m_jump.m_label)));
    }

    template<PtrTag tag>
    CodeLocationLabel<tag> locationOf(Label label)
    {
        return CodeLocationLabel<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(label.m_label)));
    }

    template<PtrTag tag>
    CodeLocationDataLabelPtr<tag> locationOf(DataLabelPtr label)
    {
        return CodeLocationDataLabelPtr<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(label.m_label)));
    }

    template<PtrTag tag>
    CodeLocationDataLabel32<tag> locationOf(DataLabel32 label)
    {
        return CodeLocationDataLabel32<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(label.m_label)));
    }

    template<PtrTag tag>
    CodeLocationDataLabelCompact<tag> locationOf(DataLabelCompact label)
    {
        return CodeLocationDataLabelCompact<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(label.m_label)));
    }

    template<PtrTag tag>
    CodeLocationConvertibleLoad<tag> locationOf(ConvertibleLoadLabel label)
    {
        return CodeLocationConvertibleLoad<tag>(MacroAssembler::getLinkerAddress<tag>(code(), applyOffset(label.m_label)));
    }

    // This method obtains the return address of the call, given as an offset from
    // the start of the code.
    unsigned returnAddressOffset(Call call)
    {
        call.m_label = applyOffset(call.m_label);
        return MacroAssembler::getLinkerCallReturnOffset(call);
    }

    uint32_t offsetOf(Label label)
    {
        return applyOffset(label.m_label).m_offset;
    }

    unsigned offsetOf(PatchableJump jump)
    {
        return applyOffset(jump.m_jump.m_label).m_offset;
    }

    // Upon completion of all patching 'FINALIZE_CODE()' should be called once to
    // complete generation of the code. Alternatively, call
    // finalizeCodeWithoutDisassembly() directly if you have your own way of
    // displaying disassembly.

    template<PtrTag tag>
    CodeRef<tag> finalizeCodeWithoutDisassembly()
    {
        return finalizeCodeWithoutDisassemblyImpl().template retagged<tag>();
    }

    template<PtrTag tag, typename... Args>
    CodeRef<tag> finalizeCodeWithDisassembly(bool dumpDisassembly, const char* format, Args... args)
    {
        ALLOW_NONLITERAL_FORMAT_BEGIN
        IGNORE_WARNINGS_BEGIN("format-security")
        return finalizeCodeWithDisassemblyImpl(dumpDisassembly, format, args...).template retagged<tag>();
        IGNORE_WARNINGS_END
        ALLOW_NONLITERAL_FORMAT_END
    }

    template<PtrTag tag>
    CodePtr<tag> trampolineAt(Label label)
    {
        return CodePtr<tag>(MacroAssembler::AssemblerType_T::getRelocatedAddress(code(), applyOffset(label.m_label)));
    }

    void* debugAddress()
    {
        return m_code.dataLocation();
    }

    size_t size() const { return m_size; }

    bool wasAlreadyDisassembled() const { return m_alreadyDisassembled; }
    void didAlreadyDisassemble() { m_alreadyDisassembled = true; }

private:
    JS_EXPORT_PRIVATE CodeRef<LinkBufferPtrTag> finalizeCodeWithoutDisassemblyImpl();
    JS_EXPORT_PRIVATE CodeRef<LinkBufferPtrTag> finalizeCodeWithDisassemblyImpl(bool dumpDisassembly, const char* format, ...) WTF_ATTRIBUTE_PRINTF(3, 4);

#if ENABLE(BRANCH_COMPACTION)
    int executableOffsetFor(int location)
    {
        // Returning 0 in this case works because at location <
        // sizeof(int32_t), no compaction could have happened before this
        // point as the assembler could not have placed a branch instruction
        // within this space that required compaction.
        if (location < static_cast<int>(sizeof(int32_t)))
            return 0;
        return bitwise_cast<int32_t*>(m_assemblerStorage.buffer())[location / sizeof(int32_t) - 1];
    }
#endif

    template <typename T> T applyOffset(T src)
    {
#if ENABLE(BRANCH_COMPACTION)
        src.m_offset -= executableOffsetFor(src.m_offset);
#endif
        return src;
    }

    // Keep this private! - the underlying code should only be obtained externally via finalizeCode().
    void* code()
    {
        return m_code.dataLocation();
    }

    void allocate(MacroAssembler&, void* ownerUID, JITCompilationEffort);

    JS_EXPORT_PRIVATE void linkCode(MacroAssembler&, void* ownerUID, JITCompilationEffort);
#if ENABLE(BRANCH_COMPACTION)
    template <typename InstructionType>
    void copyCompactAndLinkCode(MacroAssembler&, void* ownerUID, JITCompilationEffort);
#endif

    void performFinalization();

#if DUMP_LINK_STATISTICS
    static void dumpLinkStatistics(void* code, size_t initialSize, size_t finalSize);
#endif

#if DUMP_CODE
    static void dumpCode(void* code, size_t);
#endif

    RefPtr<ExecutableMemoryHandle> m_executableMemory;
    size_t m_size;
#if ENABLE(BRANCH_COMPACTION)
    AssemblerData m_assemblerStorage;
    bool m_shouldPerformBranchCompaction { true };
#endif
    bool m_didAllocate;
#ifndef NDEBUG
    bool m_completed;
#endif
    bool m_alreadyDisassembled { false };
    MacroAssemblerCodePtr<LinkBufferPtrTag> m_code;
    Vector<RefPtr<SharedTask<void(LinkBuffer&)>>> m_linkTasks;
};

#if OS(LINUX)
#define FINALIZE_CODE_IF(condition, linkBufferReference, resultPtrTag, ...)  \
    (UNLIKELY((condition))                                              \
        ? (linkBufferReference).finalizeCodeWithDisassembly<resultPtrTag>(true, __VA_ARGS__) \
        : (UNLIKELY(JSC::Options::logJITCodeForPerf()) \
            ? (linkBufferReference).finalizeCodeWithDisassembly<resultPtrTag>(false, __VA_ARGS__) \
            : (linkBufferReference).finalizeCodeWithoutDisassembly<resultPtrTag>()))
#else
#define FINALIZE_CODE_IF(condition, linkBufferReference, resultPtrTag, ...)  \
    (UNLIKELY((condition))                                              \
        ? (linkBufferReference).finalizeCodeWithDisassembly<resultPtrTag>(true, __VA_ARGS__) \
        : (linkBufferReference).finalizeCodeWithoutDisassembly<resultPtrTag>())
#endif

bool shouldDumpDisassemblyFor(CodeBlock*);

#define FINALIZE_CODE_FOR(codeBlock, linkBufferReference, resultPtrTag, ...)  \
    FINALIZE_CODE_IF((shouldDumpDisassemblyFor(codeBlock) || Options::asyncDisassembly()), linkBufferReference, resultPtrTag, __VA_ARGS__)

// Use this to finalize code, like so:
//
// CodeRef code = FINALIZE_CODE(linkBuffer, tag, "my super thingy number %d", number);
//
// Which, in disassembly mode, will print:
//
// Generated JIT code for my super thingy number 42:
//     Code at [0x123456, 0x234567]:
//         0x123456: mov $0, 0
//         0x12345a: ret
//
// ... and so on.
//
// Note that the format string and print arguments are only evaluated when dumpDisassembly
// is true, so you can hide expensive disassembly-only computations inside there.

#define FINALIZE_CODE(linkBufferReference, resultPtrTag, ...)  \
    FINALIZE_CODE_IF((JSC::Options::asyncDisassembly() || JSC::Options::dumpDisassembly()), linkBufferReference, resultPtrTag, __VA_ARGS__)

#define FINALIZE_DFG_CODE(linkBufferReference, resultPtrTag, ...)  \
    FINALIZE_CODE_IF((JSC::Options::asyncDisassembly() || JSC::Options::dumpDisassembly() || Options::dumpDFGDisassembly()), linkBufferReference, resultPtrTag, __VA_ARGS__)

#define FINALIZE_REGEXP_CODE(linkBufferReference, resultPtrTag, dataLogFArgumentsForHeading)  \
    FINALIZE_CODE_IF(JSC::Options::asyncDisassembly() || JSC::Options::dumpDisassembly() || Options::dumpRegExpDisassembly(), linkBufferReference, resultPtrTag, dataLogFArgumentsForHeading)

} // namespace JSC

#endif // ENABLE(ASSEMBLER)
