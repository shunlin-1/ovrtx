// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved. SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// Floor selector — vertically scrollable list of floors mounted inside a
// DraggablePanel. Emits floorSelected(index, name) on click so callers
// can decide what selection means (camera focus, visibility filter, …).
//
// The AGV USD has no floor prims, so `floors` is a placeholder model.
// Replace with real data once the backend exposes it.

import QtQuick
import QtQuick.Effects

DraggablePanel {
    id: floorPanel
    title: "Floors"
    width: 220
    height: 360
    // Outer panel is clear glass — no blur. Each row has its own
    // frosted-centre blur (see ShaderEffectSource/MultiEffect/ShaderEffect
    // stack on the row Item below). Inverts the typical iOS convention
    // (frosted panel / clear cards) → clear panel / frosted cards.
    blurEnabled: false

    // Floor model — each entry is { id, name }. Top of the list = top
    // floor of the building so the visual order matches reality.
    // Top of the list = top floor of the building so visual order
    // matches reality. Enough entries to overflow the visible area and
    // exercise the Flickable scroll.
    property var floors: [
        { id: "RF", name: "Rooftop — HVAC"        },
        { id: "L8", name: "Level 8 — Office"       },
        { id: "L7", name: "Level 7 — Drone Bay"    },
        { id: "L6", name: "Level 6 — Robotics QA"  },
        { id: "L5", name: "Level 5 — Mezzanine"    },
        { id: "L4", name: "Level 4 — Storage A"    },
        { id: "L3", name: "Level 3 — Storage B"    },
        { id: "L2", name: "Level 2 — Pick & Pack"  },
        { id: "L1", name: "Level 1 — Receiving"    },
        { id: "G",  name: "Ground — Loading Dock"  },
        { id: "B1", name: "Basement — Charging"    },
        { id: "B2", name: "Sub-Basement — Server"  }
    ]
    property int currentIndex: -1

    signal floorSelected(int index, string id, string name)

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: column
            width: flick.width
            spacing: 6

            Repeater {
                model: floorPanel.floors

                // Per-row glass card. Uses the same refraction shader as
                // the panel but with `source == sourceSharp` so there's
                // no blur — the lens + rim glow + border still work on
                // sharp content. Pulls its backdrop from the panel's
                // own `backdropSource` (compositeBackdrop), so the row
                // shows the underlying scene at its rim while the panel
                // around it stays frosted.
                Item {
                    id: row
                    required property var modelData
                    required property int index
                    width: column.width
                    height: 44

                    // Row corner radius matched to the panel's 14 px outer
                    // radius (slightly smaller so the visual nesting reads
                    // as concentric rather than mismatched).
                    property real radius: 12
                    property bool active: index === floorPanel.currentIndex
                    property bool hovered: rowMa.containsMouse

                    // Dark tints so the white card text stays
                    // high-contrast over the blurred mid-tone backdrop.
                    // ~45-55% alpha keeps the blur showing through.
                    readonly property color tintActive:  Qt.rgba(0.06, 0.18, 0.24, 0.55)
                    readonly property color tintHover:   Qt.rgba(0.10, 0.13, 0.20, 0.55)
                    readonly property color tintIdle:    Qt.rgba(0.05, 0.07, 0.12, 0.45)
                    readonly property color borderActive: "#4af0ff"
                    readonly property color borderIdle:   Qt.rgba(1.0, 1.0, 1.0, 0.18)

                    // Backdrop capture for this row. sourceRect must
                    // track row scroll (flick.contentY) AND panel drag
                    // (floorPanel.x/y) since QML binding capture can't
                    // see those through mapToItem() automatically.
                    ShaderEffectSource {
                        id: rowBg
                        anchors.fill: parent
                        sourceItem: floorPanel.backdropSource
                        sourceRect: {
                            // Force dependencies — see file header note.
                            const _cy = flick.contentY
                            const _cx = flick.contentX
                            const _px = floorPanel.x
                            const _py = floorPanel.y
                            const _w  = row.width
                            const _h  = row.height
                            return floorPanel.backdropSource
                                ? row.mapToItem(floorPanel.backdropSource,
                                                0, 0, _w, _h)
                                : Qt.rect(0, 0, _w, _h)
                        }
                        live: true
                        hideSource: false
                        visible: false
                        smooth: true
                    }

                    // Per-row Gaussian blur pass. blurMax sized so the
                    // kernel is wide relative to the row's short
                    // dimension — at 220×44 a 24-px kernel was too
                    // subtle to read in the ~26-px blurred centre band.
                    MultiEffect {
                        id: rowBlur
                        anchors.fill: parent
                        source: rowBg
                        visible: false
                        blurEnabled: true
                        blur: 1.0
                        blurMax: 56
                        brightness: 0.05
                        saturation: 0.14
                        autoPaddingEnabled: false
                        layer.enabled: true
                        layer.smooth: true
                    }

                    // Glass shader. source = blurred centre, sourceSharp
                    // = raw rim. Each row reads as a tiny frosted-glass
                    // card with a lensed bevel — same architecture as
                    // the panel had before, just at row scale.
                    ShaderEffect {
                        anchors.fill: parent
                        visible: floorPanel.backdropSource !== null
                        fragmentShader: Qt.resolvedUrl("refraction.frag.qsb")
                        property variant source: rowBlur
                        property variant sourceSharp: rowBg
                        property size  itemSize: Qt.size(row.width, row.height)
                        property real  cornerRadius: row.radius
                        // Bounded cubic, rim-concentrated.
                        property real  refractWidth: 12
                        property real  refractStrength: 5
                        property real  chromaticDispersion: 0.3
                        property real  noiseFrequency: 0
                        property real  noiseStrength: 0
                        property color tintColor: row.active
                            ? row.tintActive
                            : row.hovered ? row.tintHover : row.tintIdle
                        property real  rimWidth: 5
                        property real  rimBrightness: 0.22
                        property real  rimSpecular: 0
                        property real  rimLightX: 0
                        property real  rimLightY: -1
                        property real  borderWidth: 1
                        property color borderColor: row.active
                            ? row.borderActive
                            : row.borderIdle
                        Behavior on tintColor   { ColorAnimation { duration: 140 } }
                        Behavior on borderColor { ColorAnimation { duration: 140 } }
                    }

                    // Foreground content — badge + name on top of glass.
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 28; height: 22; radius: 6
                            antialiasing: true
                            color: row.active
                                ? Qt.rgba(0.04, 0.15, 0.19, 0.55)
                                : Qt.rgba(0.0, 0.0, 0.0, 0.30)
                            border.width: 1
                            border.color: row.active
                                ? "#4af0ff"
                                : Qt.rgba(1.0, 1.0, 1.0, 0.18)
                            Text {
                                anchors.centerIn: parent
                                text: row.modelData.id
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.family: "Consolas"
                                color: row.active ? "#4af0ff" : "#a3deff"
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.modelData.name
                            font.pixelSize: 12
                            font.weight: row.active ? Font.DemiBold : Font.Normal
                            color: row.active ? "#f0f4ff" : "#dbe4f5"
                            elide: Text.ElideRight
                            width: row.width - 70
                        }
                    }

                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            floorPanel.currentIndex = row.index
                            floorPanel.floorSelected(
                                row.index, row.modelData.id, row.modelData.name)
                        }
                    }
                }
            }
        }

        // No scroll indicator — iOS-style. Wheel + drag still flick the
        // Flickable; users discover scrollability when content overflows.
    }
}
