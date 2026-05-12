// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "frame_image_provider.h"

#include <QMutexLocker>

namespace agv {

FrameImageProvider::FrameImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image) {
    // Tiny transparent placeholder so the first QML render before any
    // ovrtx frame arrives doesn't show a "missing image" warning.
    image_ = QImage(1, 1, QImage::Format_RGBA8888);
    image_.fill(Qt::transparent);
}

// [snippet:provider-update]
void FrameImageProvider::update(const std::uint8_t* rgba,
                                int width, int height) {
    // Wrap-then-copy: QImage's (data, w, h, bpl, fmt) ctor is non-owning,
    // so we must deep-copy before the worker thread reuses the buffer.
    QImage frame(rgba, width, height, width * 4,
                 QImage::Format_RGBA8888);
    QImage owned = frame.copy();
    QMutexLocker lock(&mutex_);
    image_ = std::move(owned);
}
// [/snippet:provider-update]

QImage FrameImageProvider::requestImage(const QString& /*id*/,
                                        QSize* size,
                                        const QSize& /*requested_size*/) {
    QMutexLocker lock(&mutex_);
    if (size) *size = image_.size();
    return image_;
}

}  // namespace agv
