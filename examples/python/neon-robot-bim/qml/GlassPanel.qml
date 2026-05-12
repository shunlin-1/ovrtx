// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Solid-glass panel — single Rectangle with native gradient + border.
//
// Earlier versions used a stacked outer-stroked + inner-filled rectangle
// pair. Qt 6's outer-radius/inner-radius corner geometries don't align
// perfectly which produced a visible "off" white stroke at panel corners,
// and the 1 px specular highlight that snapped to `panelRadius` left a
// gap where the corner arc began. Collapsing to a single Rectangle whose
// `gradient`, `border`, and `radius` are all native to that one item
// removes the geometry mismatch — Qt antialiases all three together.

import QtQuick

Rectangle {
    id: root

    // API kept compatible with the previous blurred GlassPanel.
    property Item  backdrop:           null
    property color tint:        "#cc141923"   // bottom of the gradient
    property color tintTop:     "#dd1d2638"   // top
    property color borderColor: "#33ffffff"   // subtler — was 0x55, looked harsh
    property real  borderWidth: 1
    property real  panelRadius: 18
    property real  blurAmount:  1.0

    radius: panelRadius
    border.color: borderColor
    border.width: borderWidth
    antialiasing: true

    gradient: Gradient {
        orientation: Gradient.Vertical
        GradientStop { position: 0.0; color: tintTop }
        GradientStop { position: 1.0; color: tint }
    }

    // Mouse swallow — accepts all buttons so right/middle drags on
    // a panel don't reach the backdrop's camera-orbit handler.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        hoverEnabled: false
        propagateComposedEvents: false
    }
}
