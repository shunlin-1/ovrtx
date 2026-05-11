// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// AGV viewer window — minimal layout:
//   1. backdrop  — live ovrtx render (Image bound to QQuickImageProvider)
//   2. top pill  — title + frame counter
//   3. tool bar  — neumorph buttons bottom-left (placeholder)
//
// Camera control follows the DCC convention used by Maya / Houdini /
// Blender / Revit:
//   Right-button drag    : orbit camera
//   Mouse wheel          : dolly (zoom)
//   Left button          : reserved for future picking / selection
// The scene-area cursor stays as a plain arrow so users don't read the
// whole backdrop as a "drag handle".

import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 1600
    height: 1000
    visible: true
    color: "#0a0e18"
    title: "AGV Viewer"

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

        MouseArea {
            id: cameraCapture
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            hoverEnabled: false
            cursorShape: Qt.ArrowCursor

            // Track press position so a tiny wiggle during click still
            // counts as a click rather than a drag. >5 px travel
            // promotes the gesture to "drag" and suppresses the pick.
            property real lastX: 0
            property real lastY: 0
            property real pressX: 0
            property real pressY: 0
            property bool dragged: false

            onPressed: (mouse) => {
                lastX = mouse.x; lastY = mouse.y
                pressX = mouse.x; pressY = mouse.y
                dragged = false
            }
            onPositionChanged: (mouse) => {
                if (pressed && ovrtxBackend
                        && (mouse.buttons & Qt.RightButton)) {
                    ovrtxBackend.orbit(mouse.x - lastX, mouse.y - lastY)
                    lastX = mouse.x
                    lastY = mouse.y
                }
                // Promote left-button drag past 5 px to "dragged" so
                // the release isn't interpreted as a pick.
                if (Math.abs(mouse.x - pressX) > 5
                        || Math.abs(mouse.y - pressY) > 5) {
                    dragged = true
                }
            }
            onReleased: (mouse) => {
                if (mouse.button === Qt.LeftButton
                        && !dragged
                        && ovrtxBackend
                        && width > 0 && height > 0) {
                    ovrtxBackend.pick(mouse.x / width, mouse.y / height)
                }
            }
            onWheel: (wheel) => {
                if (ovrtxBackend) {
                    ovrtxBackend.zoom(wheel.angleDelta.y / 120.0)
                }
            }
        }
    }

    // ─── Top: title + frame counter pill ────────────────────────────
    Rectangle {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 20
        width: titleRow.implicitWidth + 36
        height: 40
        radius: 20
        color: "#aa1d2638"
        border.color: "#33ffffff"
        border.width: 1
        antialiasing: true

        Row {
            id: titleRow
            anchors.centerIn: parent
            spacing: 14

            Rectangle {
                width: 8; height: 8; radius: 4
                color: "#4af0ff"
                anchors.verticalCenter: parent.verticalCenter
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    NumberAnimation { from: 0.4; to: 1.0; duration: 800 }
                    NumberAnimation { from: 1.0; to: 0.4; duration: 800 }
                }
            }
            Text {
                text: "AGV Viewer"
                color: "#f0f4ff"
                font.pixelSize: 14
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                width: 1; height: 16
                color: "#33ffffff"
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: ovrtxBackend
                    ? "frame " + ovrtxBackend.frameCounter
                    : "—"
                color: "#4af0ff"
                font.pixelSize: 12
                font.family: "Consolas"
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // ─── Bottom-left: pick-mode toolbar ─────────────────────────────
    // Two mode buttons (Neon / Xray) drive how a left-click on a mesh
    // overrides its OmniPBR shader. The currently-active mode is
    // highlighted cyan.
    Row {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.bottomMargin: 24
        spacing: 12

        Repeater {
            model: [
                { glyph: "✦", label: "Neon",       mode: "neon" },
                { glyph: "◐", label: "Xray",       mode: "xray" },
                { glyph: "◑", label: "Xray-Light", mode: "xray-light" },
            ]
            Rectangle {
                required property var modelData
                property bool active: ovrtxBackend
                    && ovrtxBackend.pickMode === modelData.mode
                property bool hovered: ma.containsMouse
                property bool pressed: ma.pressed
                width: 96; height: 56; radius: 14
                color: pressed ? "#161a26"
                       : (active ? "#1a3744" : "#1a1f2c")
                border.width: 1
                border.color: active ? "#4af0ff"
                              : (pressed ? "#0a0e18" : "#222837")

                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Text {
                        text: modelData.glyph
                        font.pixelSize: 18
                        color: active ? "#4af0ff"
                               : (hovered ? "#a3deff" : "#d0d8e8")
                        anchors.verticalCenter: parent.verticalCenter
                        Behavior on color { ColorAnimation { duration: 160 } }
                    }
                    Text {
                        text: modelData.label
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        color: active ? "#4af0ff"
                               : (hovered ? "#a3deff" : "#d0d8e8")
                        anchors.verticalCenter: parent.verticalCenter
                        Behavior on color { ColorAnimation { duration: 160 } }
                    }
                }

                MouseArea {
                    id: ma
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        if (ovrtxBackend) {
                            ovrtxBackend.setPickMode(modelData.mode)
                        }
                    }
                }
            }
        }
    }
}
