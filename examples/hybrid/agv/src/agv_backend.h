// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "orbit_camera.h"
#include "pick_table.h"

namespace agv {

class FrameImageProvider;
class MaterialOverrides;

// QObject exposed to QML as `agvBackend`. Owns the ovrtx renderer +
// worker thread. UI thread calls orbit()/zoom(); worker thread drives
// ovrtx_step and pushes frames into FrameImageProvider.
//
// Property frameCounter bumps on every rendered frame — QML binds the
// Image source to "image://agv/frame/" + frameCounter so the provider
// is re-queried each time.
class AgvBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(int frameCounter READ frameCounter NOTIFY frameChanged)
    Q_PROPERTY(QString pickMode READ pickMode WRITE setPickMode NOTIFY pickModeChanged)

    // Global "X-ray Neon" slider (0..1). One value drives all unique
    // materials' opacity_constant + emissive_intensity simultaneously.
    Q_PROPERTY(double buildingXrayNeon READ buildingXrayNeon
               WRITE setBuildingXrayNeon NOTIFY buildingXrayNeonChanged)

    // Y-axis clipping slider — hides meshes above a world-Y threshold.
    Q_PROPERTY(double yClipValue READ yClipValue
               WRITE setYClipValue NOTIFY yClipValueChanged)
    Q_PROPERTY(double yClipMin READ yClipMin CONSTANT)
    Q_PROPERTY(double yClipMax READ yClipMax CONSTANT)

    // Last picked mesh's human-readable material name — surfaced in QML
    // as a HUD tag so the user can see which material a click landed on.
    Q_PROPERTY(QString lastPickedMaterial READ lastPickedMaterial
               NOTIFY lastPickedMaterialChanged)

    // True per-fragment "section clip" mode. When enabled, every mesh
    // is rebound to the custom MDL clip material and the Y-clip slider
    // drives `inputs:cut_height_y` instead of the per-mesh visibility.
    Q_PROPERTY(bool sectionClipEnabled READ sectionClipEnabled
               WRITE setSectionClipEnabled NOTIFY sectionClipEnabledChanged)

public:
    AgvBackend(FrameImageProvider* provider,
               std::string sidecar_usd_path,
               char up_axis,
               double initial_distance,
               std::vector<PickEntry> pick_table,
               QObject* parent = nullptr);
    ~AgvBackend() override;

    int frameCounter() const { return frame_counter_.load(); }
    QString pickMode() const;

    double buildingXrayNeon() const { return building_xray_neon_.load(); }
    double yClipValue() const       { return y_clip_value_.load(); }
    double yClipMin() const         { return y_clip_min_; }
    double yClipMax() const         { return y_clip_max_; }
    bool   sectionClipEnabled() const { return section_clip_enabled_.load(); }
    QString lastPickedMaterial() const;

    void stop();

public slots:
    void orbit(double dx, double dy);
    void zoom(double ticks);
    // Click in window-normalised coords (0..1). Queued for the worker
    // thread which does the actual ray test + material toggle.
    void pick(double x_frac, double y_frac);
    void setPickMode(const QString& mode);

    // Global "X-ray Neon" — single value in [0..1].
    // 0 = original look, 1 = full holographic cyan x-ray on every material.
    void setBuildingXrayNeon(double v);

    // Y-axis clip slider — world-Y threshold. When section clip is
    // OFF, drives per-mesh `visibility` toggling (the predicate at
    // should_hide_at_y_clip decides which meshes hide). When section
    // clip is ON, drives `inputs:cut_height_y` on the custom MDL clip
    // material for true per-fragment world-Y discard.
    void setYClipValue(double v);

    // Toggle the section-clip mode. ON rebinds every mesh to the
    // /AgvLooks/SectionClip material declared in the runtime overlay,
    // OFF restores the original Material prim bindings.
    void setSectionClipEnabled(bool enabled);

signals:
    void frameChanged(int counter);
    void pickModeChanged();
    void buildingXrayNeonChanged();
    void yClipValueChanged();
    void lastPickedMaterialChanged();
    void sectionClipEnabledChanged();

private:
    void runWorker();   // ovrtx render loop

    FrameImageProvider* provider_;       // not owned
    std::string sidecar_path_;

    // Camera state — shared between UI and worker threads.
    std::mutex camera_mutex_;
    OrbitCamera camera_;
    bool camera_dirty_ = true;

    // UI state for the Neon / Xray / Xray-Light toolbar buttons.
    // QString is touched from the Qt UI thread for signal emission;
    // mode_atomic_ is the lock-free copy the worker reads.
    // Encoding: 0=neon, 1=xray, 2=xray-light, -1=invalid (treated as no-op).
    QString pick_mode_ = QStringLiteral("xray");
    std::atomic<int> mode_atomic_{1};  // matches pick_mode_ default

    // Pending picks queued by the UI thread; drained by the worker.
    // `mutable` so lastPickedMaterial() const can lock it for the HUD read.
    mutable std::mutex picks_mutex_;
    std::vector<std::pair<double, double>> pending_picks_;

    // Pick table built once at startup (from pick_collector_bin.py)
    // and never mutated again — safe to read from the worker without
    // a lock as long as the ctor populated it before the worker started.
    std::vector<PickEntry> pick_table_;

    // Aggregate scene Y-range, computed from pick_table_ once.
    // QML uses these to set the Y-clip slider's min/max.
    double y_clip_min_ = 0.0;
    double y_clip_max_ = 0.0;

    // Slider values — atomic so QML thread can write while worker reads.
    // Worker checks `*_dirty_` flags to know when to apply changes.
    std::atomic<double> building_xray_neon_{0.0};
    std::atomic<bool>   xray_neon_dirty_{false};
    std::atomic<double> y_clip_value_{0.0};
    std::atomic<bool>   y_clip_dirty_{false};

    // Section clip (true per-fragment cut via custom MDL).
    std::atomic<bool>   section_clip_enabled_{false};
    std::atomic<bool>   section_clip_dirty_{false};

    // Last picked mesh's material name — written by the worker on hit,
    // read by QML for HUD display. Guarded by picks_mutex_.
    QString last_picked_material_;

    std::atomic<int> frame_counter_{0};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

}  // namespace agv
