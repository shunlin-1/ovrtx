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
// worker thread. UI thread calls orbit()/zoom()/pick(); worker thread
// drives ovrtx_step and pushes frames into FrameImageProvider.
//
// Stripped back to the minimum viable surface for debugging the
// per-mesh material toggle path (Neon / Xray / Xray-Light buttons).
// Slider-driven features (global X-ray Neon, Y-clip, section clip)
// were removed; see git history for the previous WIP if you want
// them back.
class AgvBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(int frameCounter READ frameCounter NOTIFY frameChanged)
    Q_PROPERTY(QString pickMode READ pickMode WRITE setPickMode NOTIFY pickModeChanged)

    // Last picked mesh's human-readable material name — surfaced in QML
    // as a HUD tag so the user can see which material a click landed on.
    Q_PROPERTY(QString lastPickedMaterial READ lastPickedMaterial
               NOTIFY lastPickedMaterialChanged)

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
    QString lastPickedMaterial() const;

    void stop();

public slots:
    void orbit(double dx, double dy);
    void zoom(double ticks);
    // Click in window-normalised coords (0..1). Queued for the worker
    // thread which does the actual ray test + material toggle.
    void pick(double x_frac, double y_frac);
    void setPickMode(const QString& mode);

signals:
    void frameChanged(int counter);
    void pickModeChanged();
    void lastPickedMaterialChanged();

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

    // Last picked mesh's material name — written by the worker on hit,
    // read by QML for HUD display. Guarded by picks_mutex_.
    QString last_picked_material_;

    std::atomic<int> frame_counter_{0};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

}  // namespace agv
