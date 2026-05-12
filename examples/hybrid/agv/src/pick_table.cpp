// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#include "pick_table.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace agv {

namespace {
constexpr std::uint32_t kMagic       = 0x50564741;  // "AGVP"
constexpr std::uint32_t kVersionMin  = 1;
constexpr std::uint32_t kVersionMax  = 2;

// Read N bytes into `dst` or return false. We never trust the file's
// claimed sizes: every read is bounded and checked.
bool read_exact(std::ifstream& in, void* dst, std::size_t n) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in.gcount()) == n;
}

template <typename T>
bool read_pod(std::ifstream& in, T& out) {
    return read_exact(in, &out, sizeof(T));
}

bool read_len_string(std::ifstream& in, std::string& out) {
    std::uint32_t len = 0;
    if (!read_pod(in, len)) return false;
    // Defensive cap — a single path > 64 KB indicates corruption,
    // long before it indicates a real prim path that exotic.
    if (len > 65536) return false;
    out.resize(len);
    return len == 0 || read_exact(in, out.data(), len);
}
}  // namespace

// [snippet:pick-table-load]
std::vector<PickEntry> load_pick_table(const std::string& bin_path) {
    std::vector<PickEntry> entries;
    std::ifstream in(bin_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[picks] failed to open %s\n", bin_path.c_str());
        return entries;
    }

    std::uint32_t magic = 0, version = 0, mesh_count = 0, reserved = 0;
    if (!read_pod(in, magic) || !read_pod(in, version) ||
        !read_pod(in, mesh_count) || !read_pod(in, reserved)) {
        std::fprintf(stderr, "[picks] short header\n");
        return entries;
    }
    if (magic != kMagic) {
        std::fprintf(stderr, "[picks] bad magic 0x%08x (want 0x%08x)\n",
                     magic, kMagic);
        return entries;
    }
    if (version < kVersionMin || version > kVersionMax) {
        std::fprintf(stderr,
                     "[picks] unsupported format version %u (want %u..%u)\n",
                     version, kVersionMin, kVersionMax);
        return entries;
    }
    const bool has_material_name = (version >= 2);

    entries.reserve(mesh_count);
    for (std::uint32_t i = 0; i < mesh_count; ++i) {
        PickEntry e;
        if (!read_len_string(in, e.mesh_path) ||
            !read_len_string(in, e.shader_path)) {
            std::fprintf(stderr, "[picks] truncated at entry %u (paths)\n", i);
            return entries;
        }
        if (has_material_name) {
            if (!read_len_string(in, e.material_name)) {
                std::fprintf(stderr,
                             "[picks] truncated at entry %u (material_name)\n",
                             i);
                return entries;
            }
        }
        if (!read_pod(in, e.bb_min) || !read_pod(in, e.bb_max) ||
            !read_pod(in, e.orig_color) || !read_pod(in, e.orig_intensity)) {
            std::fprintf(stderr, "[picks] truncated at entry %u (geom)\n", i);
            return entries;
        }
        entries.push_back(std::move(e));
    }
    std::fprintf(stderr,
                 "[picks] loaded %zu meshes from %s (v%u)\n",
                 entries.size(), bin_path.c_str(), version);
    return entries;
}
// [/snippet:pick-table-load]

}  // namespace agv
