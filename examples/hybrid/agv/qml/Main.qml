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
import QtQuick.Controls  // Slider for the global X-ray Neon + Y-clip controls

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
                id: viewportImage
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                asynchronous: false
                cache: false
                source: ovrtxBackend
                    ? "image://ovrtx/frame?counter=" + ovrtxBackend.frameCounter
                    : ""

                // Post-process: Sobel edge-detect → outline overlay.
                // layer.enabled routes this Image through an off-screen
                // FBO; layer.effect runs outline.frag.qsb over it
                // before compositing back. No C++ change needed — it's
                // a pure QML/shader feature operating on the LdrColor
                // we already pull from ovrtx.
                layer.enabled: outlineToggle.checked
                layer.effect: ShaderEffect {
                    fragmentShader: Qt.resolvedUrl("outline.frag.qsb")
                    // 1 / source-resolution. Hardcoded to ovrtx's
                    // 1920×1080 — bump if you change RENDER_WIDTH/_HEIGHT
                    // in sidecar.h.
                    property vector2d pixelStep:
                        Qt.vector2d(1.0 / 1920.0, 1.0 / 1080.0)
                    property real outlineStrength: outlineStrengthSlider.value
                    property real threshold: outlineThresholdSlider.value
                    property real thickness: outlineThicknessSlider.value
                    property color outlineColor: "#000000"
                }
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

        // ─── Bottom-center: global sliders (X-ray Neon + Y-clip) ────
        // Both drive batched ovrtx writes through the backend so the
        // whole scene responds in real time as you drag.
        Column {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 32
            spacing: 10
            width: 360

            // X-ray Neon: 0 = original, 1 = full holographic cyan.
            Rectangle {
                width: parent.width; height: 44; radius: 10
                color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#d6e4f5"; font.pixelSize: 11; font.family: "Consolas"
                    text: "X-RAY NEON  " +
                          (xrayNeonSlider.value * 100).toFixed(0) + "%"
                }
                Slider {
                    id: xrayNeonSlider
                    anchors.right: parent.right; anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.70           // wider = slower
                    from: 0.0; to: 1.0; value: 0.0
                    stepSize: 0.005                       // 200 discrete steps
                    onValueChanged: if (ovrtxBackend)
                        ovrtxBackend.setBuildingXrayNeon(value)
                }
            }

            // Y-clip — TWO modes selectable via the SECTION switch:
            //   OFF (default) → per-mesh visibility hide (binary).
            //   ON            → custom MDL clip material rebound to
            //                   every mesh; slider drives cut_height_y
            //                   for true per-fragment world-Y discard.
            //                   Walls show their interior cross-section.
            Rectangle {
                width: parent.width; height: 44; radius: 10
                color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
                Switch {
                    id: sectionClipToggle
                    anchors.left: parent.left; anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    checked: false
                    onCheckedChanged: if (ovrtxBackend)
                        ovrtxBackend.sectionClipEnabled = checked
                }
                Text {
                    anchors.left: sectionClipToggle.right; anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#d6e4f5"; font.pixelSize: 11; font.family: "Consolas"
                    text: (sectionClipToggle.checked ? "SECTION " : "Y-CLIP  ") +
                          (yClipSlider.value * 100).toFixed(0) + "%"
                }
                Slider {
                    id: yClipSlider
                    anchors.right: parent.right; anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.55
                    from: 0.0
                    to:   1.0
                    value: 0.0
                    stepSize: 0.005
                    onValueChanged: {
                        if (!ovrtxBackend) return
                        const top = ovrtxBackend.yClipMax
                        const bot = ovrtxBackend.yClipMin
                        ovrtxBackend.setYClipValue(top - value * (top - bot))
                    }
                }
            }

            // ── Outline post-process (Sobel edge-detect on LdrColor) ──
            // Toggle + 3 sliders. All QML/shader, no C++ involved —
            // operates on the rendered frame as a post-pass.
            Rectangle {
                width: parent.width; height: 44; radius: 10
                color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
                Switch {
                    id: outlineToggle
                    anchors.left: parent.left; anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    checked: false
                }
                Text {
                    anchors.left: outlineToggle.right; anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#d6e4f5"; font.pixelSize: 11; font.family: "Consolas"
                    text: "OUTLINE  " + (outlineToggle.checked ? "ON" : "off")
                }
            }

            // Outline strength / threshold / thickness sliders — only
            // visible when the outline toggle is on, to keep the chrome
            // small when post-processing is disabled.
            Rectangle {
                visible: outlineToggle.checked
                width: parent.width; height: 44; radius: 10
                color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#d6e4f5"; font.pixelSize: 11; font.family: "Consolas"
                    text: "  ↳ STR " + (outlineStrengthSlider.value * 100).toFixed(0) + "%"
                }
                Slider {
                    id: outlineStrengthSlider
                    anchors.right: parent.right; anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.70
                    from: 0.0; to: 1.0; value: 1.0
                    stepSize: 0.01
                }
            }
            Rectangle {
                visible: outlineToggle.checked
                width: parent.width; height: 44; radius: 10
                color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#d6e4f5"; font.pixelSize: 11; font.family: "Consolas"
                    text: "  ↳ THR " + outlineThresholdSlider.value.toFixed(2)
                }
                Slider {
                    id: outlineThresholdSlider
                    anchors.right: parent.right; anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.70
                    from: 0.01; to: 0.40; value: 0.08
                    stepSize: 0.005
                }
            }
            Rectangle {
                visible: outlineToggle.checked
                width: parent.width; height: 44; radius: 10
                color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#d6e4f5"; font.pixelSize: 11; font.family: "Consolas"
                    text: "  ↳ PX  " + outlineThicknessSlider.value.toFixed(1)
                }
                Slider {
                    id: outlineThicknessSlider
                    anchors.right: parent.right; anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.70
                    from: 0.5; to: 4.0; value: 1.0
                    stepSize: 0.05
                }
            }
        }

        // ─── Bottom-right: last-picked material HUD ─────────────────
        Rectangle {
            id: pickedMaterialPill
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 24
            visible: ovrtxBackend
                  && ovrtxBackend.lastPickedMaterial !== ""
            radius: 14
            color: "#aa1d2638"; border.color: "#33ffffff"; border.width: 1
            width: pickedMaterialText.width + 28
            height: pickedMaterialText.height + 18
            Text {
                id: pickedMaterialText
                anchors.centerIn: parent
                color: "#4af0ff"
                font.pixelSize: 12
                font.family: "Consolas"
                text: ovrtxBackend
                    ? "MATERIAL: " + ovrtxBackend.lastPickedMaterial
                    : ""
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
