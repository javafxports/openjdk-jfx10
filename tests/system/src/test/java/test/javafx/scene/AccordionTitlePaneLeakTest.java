/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */


package test.javafx.scene;

import javafx.application.Application;
import javafx.application.Platform;
import javafx.scene.Scene;
import javafx.scene.control.Accordion;
import javafx.scene.control.Button;
import javafx.scene.control.TitledPane;
import javafx.scene.layout.StackPane;
import javafx.stage.Stage;
import javafx.stage.WindowEvent;

import test.util.Util;

import java.lang.ref.WeakReference;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import junit.framework.Assert;
import org.junit.AfterClass;
import org.junit.BeforeClass;
import org.junit.Test;

public class AccordionTitlePaneLeakTest extends Application {

    static private CountDownLatch startupLatch;
    static private Accordion accordion;
    static private StackPane root;
    static private Stage stage;

    @Override
    public void start(Stage primaryStage) throws Exception {
        stage = primaryStage;
        accordion = new Accordion();
        root = new StackPane(accordion);
        stage.setScene(new Scene(root));
        stage.addEventHandler(WindowEvent.WINDOW_SHOWN, e -> {
            Platform.runLater(() -> startupLatch.countDown());
        });
        stage.show();
    }

    @BeforeClass
    public static void initFX() {
        startupLatch = new CountDownLatch(1);
        new Thread(() -> Application.launch(AccordionTitlePaneLeakTest.class, (String[]) null)).start();

        try {
            if (!startupLatch.await(15, TimeUnit.SECONDS)) {
                Assert.fail("Timeout waiting for FX runtime to start");
            }
        } catch (InterruptedException ex) {
            Assert.fail("Unexpected exception: " + ex);
        }
    }

    @Test
    public void testForTitledPaneLeak() throws Exception {
        TitledPane pane = new TitledPane();
        accordion.getPanes().add(pane);
        WeakReference<TitledPane> weakRefToPane = new WeakReference<>(pane);
        pane = null;
        accordion.getPanes().clear();
        for (int i = 0; i < 10; i++) {
            System.gc();
            System.runFinalization();
            if (weakRefToPane.get() == null) {
                break;
            }
            Util.sleep(500);
        }
        // Ensure accordion's skin no longer hold a ref to titled pane.
        Assert.assertNull("Couldn't collect TitledPane", weakRefToPane.get());
    }

    @AfterClass
    public static void teardownOnce() {
        Platform.runLater(() -> {
            stage.hide();
            Platform.exit();
        });
    }
}
