#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// RenderThread runs a fixed-rate busy-spin loop on its own thread and records
// the inter-frame interval for every frame. The point is to behave like
// "ovrtx_step + Vulkan present" (CPU-side cost) so we can observe whether CEF
// activity perturbs our scheduling.
//
// The thread does NOT actually render anything — we are testing thread
// isolation and OS scheduling, not rendering.
class RenderThread {
public:
    struct Config {
        int    duration_seconds  = 10;
        double target_frame_ms   = 1000.0 / 60.0;  // ~16.667 ms
        bool   pin_to_core       = false;          // optional affinity test
        int    pin_core_index    = 0;
    };

    explicit RenderThread(Config cfg);
    ~RenderThread();

    void start();
    void join();

    unsigned long thread_id() const {
        return thread_id_.load(std::memory_order_acquire);
    }

    // Call after join().
    const std::vector<double>& interval_samples_ms() const {
        return intervals_ms_;
    }

    // Pretty-print percentile summary (and "vs target" delta) to stdout.
    void print_summary(const std::string& label) const;

    // Dump raw per-frame intervals to a CSV at `path` for offline analysis.
    void dump_csv(const std::string& path) const;

private:
    void run();

    Config              cfg_;
    std::thread         thread_;
    std::atomic<unsigned long> thread_id_{0};
    std::vector<double> intervals_ms_;
};
