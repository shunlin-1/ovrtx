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
    std::mutex picks_mutex_;
    std::vector<std::pair<double, double>> pending_picks_;

    // Pick table built once at startup (from pick_collector_bin.py)
    // and never mutated again — safe to read from the worker without
    // a lock as long as setPickTable() ran before the worker started.
    std::vector<PickEntry> pick_table_;

    std::atomic<int> frame_counter_{0};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

}  // namespace agv
