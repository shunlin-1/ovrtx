// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QMutex>

#include <cstdint>

namespace agv {

// QQuickImageProvider that the ovrtx worker thread feeds RGBA frames
// into. QML triggers requestImage() each time a new image://agv/frame/N
// URL appears (we bump N from the backend on every frame).
//
// Same role as OvrtxFrameProvider in examples/python/agv/main.py.
class FrameImageProvider : public QQuickImageProvider {
public:
    FrameImageProvider();

    // Called from the ovrtx worker thread once per frame. Copies the
    // RGBA buffer into an internal QImage under a mutex. Caller retains
    // ownership of `rgba` — we deep-copy.
    void update(const std::uint8_t* rgba, int width, int height);

    // QQuickImageProvider — runs on the Qt render thread.
    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requested_size) override;

private:
    QMutex mutex_;
    QImage image_;
};

}  // namespace agv
