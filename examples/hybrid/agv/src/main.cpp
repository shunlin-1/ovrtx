// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary
//
// C++ AGV viewer — ovrtx renders, Qt6/QML overlay on top.
// C++ port of examples/python/agv/main.py. See README.md for details.

#include "agv_backend.h"
#include "frame_image_provider.h"
#include "pick_table.h"
#include "sidecar.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QFileInfo>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

struct CliArgs {
    std::string usd_path     = AGV_DEFAULT_USD;
    std::string rendermode   = "RaytracedLighting";
    std::string qml_dir      = AGV_QML_DIR;
    double      distance     = -1.0;  // sentinel -> derive from mpu
};

[[noreturn]] static void print_help_and_quit(int code) {
    std::fprintf(stderr,
        "Usage: agv [--usd PATH] [--rendermode MODE] [--distance N] [--qml-dir DIR]\n"
        "\n"
        "  --usd PATH         AGV scene (default: %s)\n"
        "  --rendermode MODE  RaytracedLighting | PathTracing | Real-Time Path-Tracing | Minimal\n"
        "  --distance N       initial camera distance in source units\n"
        "  --qml-dir DIR      QML folder (default: %s)\n",
        AGV_DEFAULT_USD, AGV_QML_DIR);
    std::_Exit(code);
}

CliArgs parse_cli(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                print_help_and_quit(1);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--usd")              a.usd_path = need_value("--usd");
        else if (arg == "--rendermode")  a.rendermode = need_value("--rendermode");
        else if (arg == "--qml-dir")     a.qml_dir = need_value("--qml-dir");
        else if (arg == "--distance")    a.distance = std::stod(need_value("--distance"));
        else if (arg == "--help" || arg == "-h") print_help_and_quit(0);
        else {
            std::fprintf(stderr, "Unknown arg: %s\n", arg.c_str());
            print_help_and_quit(1);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);

    CliArgs cli = parse_cli(argc, argv);
    if (!std::filesystem::exists(cli.usd_path)) {
        std::fprintf(stderr, "USD file not found: %s\n", cli.usd_path.c_str());
        return 1;
    }

    agv::StageMetadata md = agv::parse_stage_metadata(cli.usd_path);
    double distance = (cli.distance > 0.0) ? cli.distance : (5.0 / md.meters_per_unit);
    std::printf("[main] Source USD: upAxis=%c mpu=%f distance=%.3f\n",
                md.up_axis, md.meters_per_unit, distance);

    std::filesystem::path sidecar =
        std::filesystem::path(cli.usd_path).parent_path() / "agv_render.usda";

    agv::SidecarConfig sc;
    sc.scene_usd_path   = cli.usd_path;
    sc.out_path         = sidecar.string();
    sc.rendermode       = cli.rendermode;
    sc.up_axis          = md.up_axis;
    sc.meters_per_unit  = md.meters_per_unit;
    if (!agv::write_sidecar(sc)) {
        std::fprintf(stderr, "Failed to write sidecar: %s\n", sc.out_path.c_str());
        return 1;
    }
    std::printf("[main] Wrote sidecar: %s\n", sc.out_path.c_str());

    // ── Pick collector subprocess ────────────────────────────────────
    // Runs the Python helper in its own ephemeral uv venv (PEP 723) so
    // usd-core doesn't conflict with ovrtx's bundled USD. Output is a
    // tiny binary (~30 KB) read by pick_table.cpp.
    std::filesystem::path picks_bin =
        std::filesystem::path(sc.out_path).replace_extension(".picks.bin");
    {
        std::string cmd = std::string("uv run ") + AGV_PICK_COLLECTOR
                        + " \"" + sc.out_path + "\""
                        + " \"" + picks_bin.string() + "\"";
        std::printf("[main] Running pick collector: %s\n", cmd.c_str());
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::fprintf(stderr,
                "[main] pick_collector_bin.py failed (rc=%d) — picking disabled\n",
                rc);
        }
    }
    std::vector<agv::PickEntry> pick_table =
        agv::load_pick_table(picks_bin.string());

    QQmlApplicationEngine engine;
    // Tell QML where to find Qt's built-in modules (QtQuick.Window,
    // QtQuick.Effects, etc.) so the user doesn't need QML_IMPORT_PATH.
    // AGV_QT_QML_DIR is baked at CMake configure time from Qt6_DIR.
    engine.addImportPath(QStringLiteral(AGV_QT_QML_DIR));

    // Names match the Python AGV viewer so its QML files port verbatim.
    auto* provider = new agv::FrameImageProvider();
    engine.addImageProvider(QStringLiteral("ovrtx"), provider);

    agv::AgvBackend backend(provider, sc.out_path, md.up_axis,
                            distance, std::move(pick_table));
    engine.rootContext()->setContextProperty(QStringLiteral("ovrtxBackend"),
                                             &backend);

    QString qml_main = QString::fromStdString(cli.qml_dir) + "/Main.qml";
    engine.load(QUrl::fromLocalFile(qml_main));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "Failed to load QML: %s\n",
                     qPrintable(qml_main));
        return 1;
    }

    QObject::connect(&app, &QGuiApplication::aboutToQuit,
                     &backend, &agv::AgvBackend::stop);

    return app.exec();
}
