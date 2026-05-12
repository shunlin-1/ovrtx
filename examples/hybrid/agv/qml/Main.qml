// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// AGV viewer window layout. Two scene-graph subtrees:
//
//   compositeBackdrop   — everything "behind" the floor panel:
//                         (1) ovrtx render Item (id: backdrop)
//                         (2) title pill
//                         (3) bottom-left pick-mode toolbar
//                         The floor panel samples this as its glass
//                         source, so dragging the panel over the
//                         toolbar shows those buttons through the
//                         frosted glass.
//
//   floorPanel          — sibling of compositeBackdrop, rendered on
//                         top. Lives outside the composite so it can
//                         safely sample it without creating a feedback
//                         loop. GlassButtons inside the toolbar keep
//                         their `backdropSource: backdrop` (the raw
//                         ovrtx render only) for the same reason —
//                         sampling compositeBackdrop from inside it
//                         would loop.
//
// Camera control follows the DCC convention (Maya / Houdini / Blender):
//   Right-button drag    — orbit camera
//   Mouse wheel          — dolly / zoom
//   Left button          — pick / select mesh (toggles current pick mode)

import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 1600
    height: 1000
    visible: true
    color: "#0a0e18"
    title: "AGV Viewer"

    // ════════════════════════════════════════════════════════════════
    // Composite backdrop — everything sampled by the floor panel's
    // glass. Rendered first so it sits visually below the panel.
    // ════════════════════════════════════════════════════════════════
    Item {
        id: compositeBackdrop
        anchors.fill: parent

        // ─── Backdrop: ovrtx render + orbit-camera mouse capture ────
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

                // Tiny wiggles during click still count as a click.
                // >5 px travel promotes to "drag" and suppresses pick.
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
                        // The ovrtx render is fixed at 1920×1080 and the
                        // Image uses PreserveAspectCrop, so window NDC
                        // isn't the same as image NDC at non-matching
                        // aspect ratios. Convert here so the ray hits the
                        // mesh under the cursor instead of one nearby.
                        const imgW = 1920, imgH = 1080
                        const scale = Math.max(width / imgW, height / imgH)
                        const shownW = imgW * scale
                        const shownH = imgH * scale
                        const cropX = (shownW - width) / 2   // ≥0 when image wider
                        const cropY = (shownH - height) / 2  // ≥0 when image taller
                        const imgXFrac = (mouse.x + cropX) / shownW
                        const imgYFrac = (mouse.y + cropY) / shownH
                        ovrtxBackend.pick(imgXFrac, imgYFrac)
                    }
                }
                onWheel: (wheel) => {
                    if (ovrtxBackend) {
                        ovrtxBackend.zoom(wheel.angleDelta.y / 120.0)
                    }
                }
            }
        }

        // ─── Top: title + frame counter pill ────────────────────────
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

        // ─── Bottom-left: pick-mode toolbar ─────────────────────────
        // Each button samples raw `backdrop` (not compositeBackdrop)
        // to avoid a feedback loop — they ARE inside compositeBackdrop.
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
                GlassButton {
                    required property var modelData
                    width: 110
                    height: 56
                    radius: 14
                    label: modelData.label
                    glyph: modelData.glyph
                    backdropSource: backdrop
                    active: ovrtxBackend
                        && ovrtxBackend.pickMode === modelData.mode
                    onClicked: {
                        if (ovrtxBackend) {
                            ovrtxBackend.setPickMode(modelData.mode)
                        }
                    }
                }
            }
        }
    }

    // ════════════════════════════════════════════════════════════════
    // Floor selector — separate subtree so its glass can sample the
    // entire compositeBackdrop (ovrtx + pill + toolbar) without
    // creating a feedback loop.
    // ════════════════════════════════════════════════════════════════
    FloorSelectPanel {
        id: floorPanel
        x: parent.width - width - 24
        y: 80
        backdropSource: compositeBackdrop
        onFloorSelected: (index, id, name) => {
            console.log("[ui] floor selected:", id, "—", name)
            // TODO: wire to ovrtxBackend once the backend exposes a
            //       floor visibility / camera-focus slot.
        }
    }
}
