// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Main BIM-viewer window. Layout order (z-stack, back to front):
//   1. backdrop — live ovrtx render (Image bound to QQuickImageProvider)
//   2. drag card     (GlassPanel, top-left)
//   3. stats panel   (GlassPanel, bottom-right)
//   4. tool buttons  (Rectangles, bottom-left)
//   5. hover buttons (Rectangles, top-center)
//   6. live dot      (Rectangle, top-right)

import QtQuick
import QtQuick.Window
import QtQuick.Effects

Window {
    id: root
    width: 1600
    height: 1000
    visible: true
    color: "#0a0e18"
    title: "neon-robot-bim — Stage 1 (no 3D yet)"

    // ─── Backdrop: live ovrtx render + orbit-camera mouse capture ───
    Item {
        id: backdrop
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            color: "#0a0e18"
        }

        Image {
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            cache: false
            source: ovrtxBackend
                ? "image://ovrtx/frame?counter=" + ovrtxBackend.frameCounter
                : ""
        }

        // Mouse capture follows the DCC convention used by Maya / Houdini /
        // Blender / Revit:
        //   Right-button drag    : orbit camera
        //   Middle-button drag   : (TODO) pan camera
        //   Mouse wheel          : zoom
        //   Left button          : reserved for object selection / picking
        //                          (passed through to children — future
        //                          "drag a cube on the floor" interactions)
        // Crucially the cursor stays as a normal arrow over the scene,
        // so users don't read the whole backdrop as a "drag handle".
        MouseArea {
            id: cameraCapture
            anchors.fill: parent
            acceptedButtons: Qt.MiddleButton | Qt.RightButton
            hoverEnabled: false
            cursorShape: Qt.ArrowCursor

            property real lastX: 0
            property real lastY: 0

            onPressed: (mouse) => { lastX = mouse.x; lastY = mouse.y }
            onPositionChanged: (mouse) => {
                if (pressed && ovrtxBackend
                        && (mouse.buttons & Qt.RightButton)) {
                    ovrtxBackend.orbit(mouse.x - lastX, mouse.y - lastY)
                    lastX = mouse.x
                    lastY = mouse.y
                }
                // TODO Middle-button pan: requires a `pan(dx, dy)` slot
                // on OvrtxBackend that nudges OrbitCamera.target along
                // the camera-local right/up vectors.
            }
            onWheel: (wheel) => {
                if (ovrtxBackend) {
                    ovrtxBackend.zoom(wheel.angleDelta.y / 120.0)
                }
            }
        }
    }

    // ─── Top-left: draggable selection card (Glass) ─────────────────
    GlassPanel {
        id: dragCard
        x: 24; y: 80
        width: 240; height: 120
        backdrop:    backdrop
        tint:        "#66283250"   // alpha ≈ 0.4 so blur shows
        tintTop:     "#553a4670"
        borderColor: "#55ffffff"

        Column {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 4
            Text {
                text: "⋮⋮  DRAG HANDLE"
                color: "#b3f0f4ff"
                font.pixelSize: 11
                font.letterSpacing: 1
            }
            Text {
                text: "Beam B-204"
                color: "#f0f4ff"
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text { text: "Material: Steel S355"; color: "#b3f0f4ff"; font.pixelSize: 12 }
            Text { text: "Length: 6.40 m";       color: "#b3f0f4ff"; font.pixelSize: 12 }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            drag.target: dragCard
            drag.minimumX: 0; drag.minimumY: 0
            drag.maximumX: root.width  - dragCard.width
            drag.maximumY: root.height - dragCard.height
        }
    }

    // ─── Bottom-right: glassmorphism stats panel ────────────────────
    GlassPanel {
        id: statsPanel
        x: root.width  - 320 - 24
        y: root.height - 200 - 24
        width: 320; height: 200
        backdrop:    backdrop
        tint:        "#66141923"   // alpha ≈ 0.4 so blur shows
        tintTop:     "#551d2638"
        borderColor: "#55ffffff"

        Column {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 6

            Row {
                spacing: 8
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: "#4af0ff"
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: "LIVE STATS"
                    color: "#cfe8ff"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Repeater {
                model: [
                    { label: "FPS",       value: "60"   },
                    { label: "Triangles", value: "2.4 M"},
                    { label: "Memory",    value: "312 MB"}
                ]
                Row {
                    required property var modelData
                    width: parent.width
                    Text {
                        text: parent.modelData.label
                        color: "#f0f4ff"
                        font.pixelSize: 13
                        width: parent.width / 2
                    }
                    Text {
                        text: parent.modelData.value
                        color: "#4af0ff"
                        font.pixelSize: 13
                        horizontalAlignment: Text.AlignRight
                        width: parent.width / 2
                    }
                }
            }
        }
    }

    // ─── Bottom-left: neumorphic tool buttons ───────────────────────
    Row {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.bottomMargin: 24
        spacing: 16

        Repeater {
            model: ["✥", "⟳", "▣"]
            Rectangle {
                required property string modelData
                property bool hovered: ma.containsMouse
                property bool pressed: ma.pressed
                width: 70; height: 70; radius: 16
                color: pressed ? "#161a26" : "#1a1f2c"
                border.width: 1
                border.color: pressed ? "#0a0e18" : "#222837"

                Text {
                    anchors.centerIn: parent
                    text: parent.modelData
                    font.pixelSize: 24
                    color: hovered ? "#4af0ff" : "#d0d8e8"
                    Behavior on color { ColorAnimation { duration: 160 } }
                }

                MouseArea {
                    id: ma
                    anchors.fill: parent
                    hoverEnabled: true
                }
            }
        }
    }

    // ─── Top-center: hover-glow primary buttons ─────────────────────
    Row {
        anchors.top: parent.top
        anchors.topMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 12

        Repeater {
            model: [
                { label: "Reset View",   primary: false },
                { label: "Layers",       primary: false },
                { label: "Run Analysis", primary: true }
            ]
            Rectangle {
                required property var modelData
                property bool hovered: hoverArea.containsMouse
                width: txt.implicitWidth + 44
                height: 40
                radius: 11

                color: modelData.primary
                    ? "transparent"
                    : (hovered ? "#384af0ff" : "#144af0ff")
                border.width: modelData.primary ? 0 : 1
                border.color: hovered ? "#994af0ff" : "#26ffffff"

                gradient: modelData.primary ? cyanPurpleGrad : null
                Gradient {
                    id: cyanPurpleGrad
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#4af0ff" }
                    GradientStop { position: 1.0; color: "#c474ff" }
                }

                Behavior on color { ColorAnimation { duration: 200 } }

                Text {
                    id: txt
                    anchors.centerIn: parent
                    text: modelData.label
                    color: modelData.primary
                        ? "#0c1020"
                        : (hovered ? "#ffffff" : "#d8f6ff")
                    font.pixelSize: 13
                    font.weight: modelData.primary ? Font.DemiBold : Font.Medium
                }

                MouseArea {
                    id: hoverArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: console.log("clicked:", modelData.label)
                }
            }
        }
    }

    // ─── Top-right: pulsing live indicator ──────────────────────────
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 24
        anchors.topMargin: 28
        width: 28; height: 28; radius: 14
        color: "#4af0ff"
        SequentialAnimation on opacity {
            loops: Animation.Infinite
            NumberAnimation { from: 0.5; to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
            NumberAnimation { from: 1.0; to: 0.5; duration: 600; easing.type: Easing.InOutQuad }
        }
    }
}
