// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Liquid-Glass HUD panel — refactor.
//
//   DraggablePanel {
//       title: "Floors"
//       backdropSource: backdrop
//       width: 240; height: 360
//       Flickable { ... }             // child → contentArea
//   }
//
// Layer stack (back → front):
//   0. shadowShape + blurPass      — outer drop shadow.
//   1. ShaderEffectSource          — live capture of backdropSource.
//   2. MultiEffect (blurPass)      — Gaussian blur → texture.
//   3. ShaderEffect (refract.frag) — refraction + dispersion + tint +
//                                    rim light + border + mask. ONE
//                                    pass replaces the old tint /
//                                    highlight / border Rectangles.
//   4. Header                      — title + grip dots + drag handle.
//   5. Content area                — default children land here.

import QtQuick
import QtQuick.Effects

Item {
    id: panel
    width: 240
    height: 320

    // ── Public API ──────────────────────────────────────────────────
    property alias title: titleLabel.text
    property alias headerHeight: header.height
    default property alias contentItemData: contentArea.data

    // Geometry
    property real   radius: 14
    property Item   backdropSource: null
    property int    blurMax: 48
    property bool   blurEnabled: true            // false = bypass Gaussian
                                                  // blur (debug / sharp lens)
    // Shape — 0 = rounded rectangle, 1 = ellipse, anything in between
    // morphs between the two shapes via SDF mix.
    property real   ellipseMode: 0.0

    // Surface
    property color  tintColor: Qt.rgba(0.12, 0.14, 0.20, 0.30)

    // Lens — bounded cubic curve (pow(1-band, 3)), rim-concentrated.
    // refractStrength stays at 18 so text under the rim doesn't fan
    // out; chromaticDispersion bumped up since the gated formula
    // (weight × dispersion) keeps the rainbow concentrated at the
    // rim band only — interior text stays clean.
    property real   refractWidth: 60
    property real   refractStrength: 18
    property real   chromaticDispersion: 0.8

    // Optional surface noise — off by default; turn up for the CSS
    // bathroom-glass / feTurbulence look.
    property real   noiseFrequency: 4.0
    property real   noiseStrength: 0.0

    // Unified rim light. Directional component disabled by default so
    // every corner is mathematically identical (only omni glow remains).
    // Set rimSpecular > 0 if you want a directional highlight back —
    // but remember to keep rimLight + shadowOffset agreeing on light
    // direction.
    // Rim light. Directional specular brought back so the top edge
    // catches a visible bright arc — that bright top crescent is the
    // visual signature of iOS-style glass pills. Light from straight
    // above (0, -1) keeps top-left and top-right corners equally lit.
    property real   rimWidth: 14
    property real   rimBrightness: 0.20                       // omni glow
    property real   rimSpecular: 0.45                         // directional
    property vector2d rimLight: Qt.vector2d(0.0, -1.0)

    // In-shader border
    property real   borderWidth: 1.0
    property color  borderColor: Qt.rgba(1, 1, 1, 0.28)

    // Outer drop shadow. shadowOffset = (0, 0) ⇒ symmetric radial halo
    // around the panel — top and bottom edges look identical, no
    // implied "light direction" anywhere. (Set offsetY > 0 if you want
    // a "light from above" cue back; you'll get top-vs-bottom
    // asymmetry as a consequence.)
    property bool   shadowEnabled: true
    property real   shadowOffsetX: 0
    property real   shadowOffsetY: 0
    property int    shadowBlurMax: 64
    property color  shadowColor: Qt.rgba(0, 0, 0, 0.50)

    // Header
    property color  headerColor: Qt.rgba(1.0, 1.0, 1.0, 0.06)

    // ── 0. Outer drop shadow ────────────────────────────────────────
    Rectangle {
        id: shadowShape
        x: panel.shadowOffsetX
        y: panel.shadowOffsetY
        width: panel.width
        height: panel.height
        radius: panel.radius
        color: panel.shadowColor
        visible: false
        layer.enabled: true
        layer.smooth: true
    }
    MultiEffect {
        anchors.fill: shadowShape
        source: shadowShape
        visible: panel.shadowEnabled
        blurEnabled: true
        blur: 1.0
        blurMax: panel.shadowBlurMax
        autoPaddingEnabled: true
        z: -1
    }

    // ── 1. Capture the backdrop region beneath this panel ───────────
    ShaderEffectSource {
        id: bgCapture
        anchors.fill: parent
        sourceItem: panel.backdropSource
        sourceRect: {
            const _x = panel.x, _y = panel.y
            const _w = panel.width, _h = panel.height
            return panel.backdropSource
                ? panel.mapToItem(panel.backdropSource, 0, 0, _w, _h)
                : Qt.rect(0, 0, _w, _h)
        }
        live: true
        hideSource: false
        visible: false
        smooth: true
    }

    // ── 2. Gaussian-blur the capture into a sampleable texture ──────
    MultiEffect {
        id: blurPass
        anchors.fill: parent
        source: bgCapture
        visible: false
        blurEnabled: panel.blurEnabled
        blur: 1.0
        blurMax: panel.blurMax
        brightness: 0.06
        saturation: 0.18
        autoPaddingEnabled: false
        layer.enabled: true
        layer.smooth: true
    }

    // ── 3. All-in-one Liquid Glass shader ───────────────────────────
    ShaderEffect {
        anchors.fill: parent
        visible: panel.backdropSource !== null
        fragmentShader: Qt.resolvedUrl("refraction.frag.qsb")
        // Two samplers: blur for the interior frost, raw for the rim
        // lens. The shader blends them by refraction weight.
        property variant source: blurPass
        property variant sourceSharp: bgCapture
        property size  itemSize: Qt.size(panel.width, panel.height)
        property real  cornerRadius: panel.radius
        // Lens
        property real  refractWidth: panel.refractWidth
        property real  refractStrength: panel.refractStrength
        property real  chromaticDispersion: panel.chromaticDispersion
        // Noise
        property real  noiseFrequency: panel.noiseFrequency
        property real  noiseStrength: panel.noiseStrength
        // Tint
        property color tintColor: panel.tintColor
        // Rim
        property real  rimWidth: panel.rimWidth
        property real  rimBrightness: panel.rimBrightness
        property real  rimSpecular: panel.rimSpecular
        property real  rimLightX: panel.rimLight.x
        property real  rimLightY: panel.rimLight.y
        // Border
        property real  borderWidth: panel.borderWidth
        property color borderColor: panel.borderColor
        // Shape
        property real  ellipseMode: panel.ellipseMode
    }

    // ── 4. Header — iOS sheet style ─────────────────────────────────
    // Drag-handle pill at top-center; title centered beneath it.
    // No solid background — the shader's glass surface shows through;
    // the pill alone is the visual anchor that says "grab here".
    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44

        // Drag handle pill (iOS drawer indicator).
        Rectangle {
            id: dragPill
            anchors.top: parent.top
            anchors.topMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            width: 38
            height: 5
            radius: 2.5
            color: dragArea.drag.active
                ? Qt.rgba(1, 1, 1, 0.55)
                : Qt.rgba(1, 1, 1, 0.30)
            antialiasing: true
            Behavior on color { ColorAnimation { duration: 140 } }
        }

        // Centered title beneath the pill.
        Text {
            id: titleLabel
            anchors.top: dragPill.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#f0f4ff"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            drag.target: panel
            drag.axis: Drag.XAndYAxis
            drag.minimumX: 0
            drag.maximumX: panel.parent ? panel.parent.width  - panel.width  : 0
            drag.minimumY: 0
            drag.maximumY: panel.parent ? panel.parent.height - panel.height : 0
        }
    }

    // ── 5. Content area (default children land here) ────────────────
    Item {
        id: contentArea
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        clip: true
    }
}
