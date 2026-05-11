// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Liquid-Glass button — refactor (matches DraggablePanel structure).
//
//   GlassButton {
//       label: "Neon"
//       glyph: "✦"
//       active: true                  // sticky-selected state
//       backdropSource: backdrop
//       onClicked: console.log("hi")
//   }
//
// Stack:
//   0. drop shadow (blurred rect)
//   1. ShaderEffectSource           — backdrop capture
//   2. MultiEffect blurPass          — Gaussian blur → texture layer
//   3. ShaderEffect refraction.frag  — refraction + dispersion + tint
//                                      + unified rim light + border
//   4. Hover shine sweep
//   5. Label + glyph

import QtQuick
import QtQuick.Effects

Item {
    id: root

    // ── Public API ──────────────────────────────────────────────────
    property string label: "Button"
    property string glyph: ""
    property bool   active: false
    property Item   backdropSource: null
    property real   radius: 14
    property int    blurMax: 48
    property bool   blurEnabled: true   // false = bypass Gaussian blur

    // Tint — driven by hover / press / active states.
    property color  accentColor: "#4af0ff"

    // Bounded cubic lens curve — rim-concentrated.
    property real   refractWidth: 18
    property real   refractStrength: 9
    // Mid-range dispersion: visible rainbow on the rim, still
    // restrained enough that label text doesn't ghost.
    property real   chromaticDispersion: 0.5

    // Noise off by default
    property real   noiseFrequency: 3.0
    property real   noiseStrength: 0.0

    // Rim light — directional specular brought back for the iOS top-
    // edge bright arc.
    property real   rimWidth: 8
    property real   rimBrightness: 0.20
    property real   rimSpecular: 0.40
    property vector2d rimLight: Qt.vector2d(0.0, -1.0)

    // Border
    property real   borderWidth: 1.0
    property color  borderColor: Qt.rgba(1, 1, 1, 0.28)

    // Drop shadow. Symmetric radial halo (no Y offset) to keep
    // top and bottom looking identical.
    property bool   shadowEnabled: true
    property real   shadowOffsetX: 0
    property real   shadowOffsetY: 0
    property int    shadowBlurMax: 32
    property color  shadowColor: Qt.rgba(0, 0, 0, 0.40)

    // Hover shine
    property bool   shineOnHover: true

    signal clicked()

    width: 140
    height: 44

    // Computed tint colour from state + active + hover.
    QtObject {
        id: _state
        readonly property color tint: root.active
            ? Qt.rgba(0.29, 0.94, 1.00, 0.20)
            : ma.pressed
                ? Qt.rgba(1, 1, 1, 0.28)
                : ma.containsMouse
                    ? Qt.rgba(1, 1, 1, 0.18)
                    : Qt.rgba(1, 1, 1, 0.12)
        readonly property color border: root.active
            ? root.accentColor
            : ma.containsMouse
                ? Qt.rgba(1, 1, 1, 0.45)
                : root.borderColor
    }

    // ── 0. Outer drop shadow ────────────────────────────────────────
    Rectangle {
        id: shadowShape
        x: root.shadowOffsetX
        y: root.shadowOffsetY
        width: root.width
        height: root.height
        radius: root.radius
        color: root.shadowColor
        visible: false
        layer.enabled: true
        layer.smooth: true
    }
    MultiEffect {
        anchors.fill: shadowShape
        source: shadowShape
        visible: root.shadowEnabled
        blurEnabled: true
        blur: 1.0
        blurMax: root.shadowBlurMax
        autoPaddingEnabled: true
        z: -1
    }

    // ── 1. Capture the backdrop region beneath us ───────────────────
    ShaderEffectSource {
        id: bgCapture
        anchors.fill: parent
        sourceItem: root.backdropSource
        sourceRect: {
            const _x = root.x, _y = root.y
            const _w = root.width, _h = root.height
            return root.backdropSource
                ? root.mapToItem(root.backdropSource, 0, 0, _w, _h)
                : Qt.rect(0, 0, _w, _h)
        }
        live: true
        hideSource: false
        visible: false
        smooth: true
    }

    // ── 2. Blur into a texture layer ────────────────────────────────
    MultiEffect {
        id: blurPass
        anchors.fill: parent
        source: bgCapture
        visible: false
        blurEnabled: root.blurEnabled
        blur: 1.0
        blurMax: root.blurMax
        brightness: 0.06
        saturation: 0.18
        autoPaddingEnabled: false
        layer.enabled: true
        layer.smooth: true
    }

    // ── 3. Liquid Glass shader ──────────────────────────────────────
    ShaderEffect {
        anchors.fill: parent
        visible: root.backdropSource !== null
        fragmentShader: Qt.resolvedUrl("refraction.frag.qsb")
        // Two samplers (interior blur + rim sharp). See DraggablePanel.
        property variant source: blurPass
        property variant sourceSharp: bgCapture
        property size  itemSize: Qt.size(root.width, root.height)
        property real  cornerRadius: root.radius
        property real  refractWidth: root.refractWidth
        property real  refractStrength: root.refractStrength
        property real  chromaticDispersion: root.chromaticDispersion
        property real  noiseFrequency: root.noiseFrequency
        property real  noiseStrength: root.noiseStrength
        property color tintColor: _state.tint
        property real  rimWidth: root.rimWidth
        property real  rimBrightness: root.rimBrightness
        property real  rimSpecular: root.active ? root.rimSpecular * 1.2
                                                : root.rimSpecular
        property real  rimLightX: root.rimLight.x
        property real  rimLightY: root.rimLight.y
        property real  borderWidth: root.borderWidth
        property color borderColor: _state.border
        Behavior on tintColor    { ColorAnimation { duration: 140 } }
        Behavior on borderColor  { ColorAnimation { duration: 140 } }
    }

    // ── 4. Hover shine sweep ────────────────────────────────────────
    Item {
        id: shineClip
        anchors.fill: parent
        clip: true
        visible: root.shineOnHover
        layer.enabled: true
        Rectangle {
            id: shine
            width: parent.width * 0.35
            height: parent.height * 3
            rotation: 20
            anchors.verticalCenter: parent.verticalCenter
            opacity: 0.0
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#00ffffff" }
                GradientStop { position: 0.5; color: "#66ffffff" }
                GradientStop { position: 1.0; color: "#00ffffff" }
            }
            x: -width
        }
    }

    // ── 5. Label + optional glyph ───────────────────────────────────
    Row {
        anchors.centerIn: parent
        spacing: root.glyph === "" ? 0 : 8

        Text {
            visible: root.glyph !== ""
            text: root.glyph
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 18
            color: root.active
                ? root.accentColor
                : ma.containsMouse ? "#a3deff" : "#d0d8e8"
            Behavior on color { ColorAnimation { duration: 160 } }
        }
        Text {
            text: root.label
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: root.active
                ? root.accentColor
                : ma.containsMouse ? "#eaf6ff" : "#f4f8ff"
            Behavior on color { ColorAnimation { duration: 160 } }
        }
    }

    // ── 6. Interaction ──────────────────────────────────────────────
    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
        onEntered: if (root.shineOnHover) shineAnim.restart()
    }

    SequentialAnimation {
        id: shineAnim
        PropertyAction { target: shine; property: "x"; value: -shine.width }
        PropertyAction { target: shine; property: "opacity"; value: 1.0 }
        NumberAnimation {
            target: shine; property: "x"
            to: root.width
            duration: 650
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: shine; property: "opacity"
            to: 0.0
            duration: 200
        }
    }
}
