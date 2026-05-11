#include "render_thread.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <numeric>

#include <windows.h>

namespace {

// QueryPerformanceCounter wrapper; std::chrono::steady_clock on MSVC also wraps
// QPC but we use it directly to make the spike's timing path obvious + auditable.
struct QpcClock {
    static double frequency_hz() {
        static const double freq = []() {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        return freq;
    }

    static int64_t now_counts() {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    }

    static double counts_to_ms(int64_t counts) {
        return (static_cast<double>(counts) / frequency_hz()) * 1000.0;
    }
};

double percentile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    size_t idx = static_cast<size_t>(static_cast<double>(sorted.size() - 1) * q);
    return sorted[idx];
}

}  // namespace

RenderThread::RenderThread(Config cfg) : cfg_(cfg) {
    intervals_ms_.reserve(static_cast<size_t>(cfg.duration_seconds * 70));
}

RenderThread::~RenderThread() {
    if (thread_.joinable()) thread_.join();
}

void RenderThread::start() {
    thread_ = std::thread(&RenderThread::run, this);
}

void RenderThread::join() {
    if (thread_.joinable()) thread_.join();
}

void RenderThread::run() {
    thread_id_.store(static_cast<unsigned long>(GetCurrentThreadId()),
                     std::memory_order_release);

    if (cfg_.pin_to_core) {
        DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << cfg_.pin_core_index;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }

    // Bump scheduling priority so OS doesn't deprioritize us under load. This
    // mirrors what a real render thread would do.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    const double freq = QpcClock::frequency_hz();
    const int64_t target_period_counts =
        static_cast<int64_t>((cfg_.target_frame_ms / 1000.0) * freq);
    const int64_t total_counts =
        static_cast<int64_t>(static_cast<double>(cfg_.duration_seconds) * freq);

    int64_t start = QpcClock::now_counts();
    int64_t last_frame = start;
    int64_t next_target = start + target_period_counts;

    while (true) {
        int64_t now = QpcClock::now_counts();
        if (now - start >= total_counts) break;

        // [snippet:option-a-busy-spin]
        // Option A: pure CPU busy-spin to next frame target. Models a CPU-bound
        // render loop. We deliberately use volatile + a real arithmetic
        // sequence so the optimiser doesn't elide the loop body.
        while (QpcClock::now_counts() < next_target) {
            volatile int x = 0;
            for (int i = 0; i < 1024; ++i) {
                x = x + i;
            }
        }
        // [/snippet:option-a-busy-spin]

        int64_t frame_end = QpcClock::now_counts();
        double interval_ms = QpcClock::counts_to_ms(frame_end - last_frame);
        intervals_ms_.push_back(interval_ms);

        last_frame = frame_end;
        next_target += target_period_counts;

        // If we fell behind by more than half a frame, resync to "now" instead
        // of accumulating debt — otherwise one stall pretends to be many.
        if (frame_end > next_target + target_period_counts / 2) {
            next_target = frame_end + target_period_counts;
        }
    }
}

void RenderThread::print_summary(const std::string& label) const {
    if (intervals_ms_.empty()) {
        std::fprintf(stdout, "[%s] no samples collected\n", label.c_str());
        return;
    }

    std::vector<double> sorted = intervals_ms_;
    std::sort(sorted.begin(), sorted.end());

    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    double mean = sum / static_cast<double>(sorted.size());

    std::fprintf(stdout, "\n=== Frame time summary [%s] ===\n", label.c_str());
    std::fprintf(stdout, "  samples:  %zu\n", sorted.size());
    std::fprintf(stdout, "  target:   %.3f ms\n", cfg_.target_frame_ms);
    std::fprintf(stdout, "  min:      %.3f ms\n", sorted.front());
    std::fprintf(stdout, "  mean:     %.3f ms\n", mean);
    std::fprintf(stdout, "  p50:      %.3f ms\n", percentile(sorted, 0.50));
    std::fprintf(stdout, "  p95:      %.3f ms\n", percentile(sorted, 0.95));
    std::fprintf(stdout, "  p99:      %.3f ms\n", percentile(sorted, 0.99));
    std::fprintf(stdout, "  p99.9:    %.3f ms\n", percentile(sorted, 0.999));
    std::fprintf(stdout, "  max:      %.3f ms\n", sorted.back());

    // Count "bad" frames: > 2x target.
    double threshold = cfg_.target_frame_ms * 2.0;
    size_t bad = std::count_if(sorted.begin(), sorted.end(),
                               [threshold](double x) { return x > threshold; });
    std::fprintf(stdout, "  > 2x target: %zu (%.2f%%)\n", bad,
                 100.0 * static_cast<double>(bad) / static_cast<double>(sorted.size()));
    std::fflush(stdout);
}

void RenderThread::dump_csv(const std::string& path) const {
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "[render-thread] failed to open %s for csv dump\n",
                     path.c_str());
        return;
    }
    out << "frame_idx,interval_ms\n";
    for (size_t i = 0; i < intervals_ms_.size(); ++i) {
        out << i << "," << intervals_ms_[i] << "\n";
    }
    std::fprintf(stdout, "[render-thread] csv written: %s\n", path.c_str());
}
