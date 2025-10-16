#pragma once
#include <chrono>
#include <string>
#include <array>
#include <atomic>
#include <cstdio>

// ─────────────────────────────────────────────
// RT-safe ElapsedTimer
// - 동적 메모리 없음 (std::vector 제거)
// - I/O 최소화 (flush 없음)
// - Lock-free / noexcept
// - 실시간 루프 내 사용 안전
// ─────────────────────────────────────────────
class ElapsedTimerRT {
public:
    explicit ElapsedTimerRT(const std::string& name = "")
        : name_(name), index_(0), count_(0) {}

    inline void startTimer() noexcept {
        start_time_ = std::chrono::steady_clock::now();
    }

    inline void stopTimer() noexcept {
        auto end_time = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time_).count();

        size_t idx = index_.fetch_add(1, std::memory_order_relaxed) % max_samples;
        samples_[idx] = elapsed_ms;
        if (count_.load(std::memory_order_relaxed) < max_samples)
            count_.fetch_add(1, std::memory_order_relaxed);
    }

    inline double latest() const noexcept {
        if (count_.load(std::memory_order_relaxed) == 0) return 0.0;
        size_t idx = (index_.load(std::memory_order_relaxed) - 1) % max_samples;
        return samples_[idx];
    }

    inline double mean() const noexcept {
        size_t c = count_.load(std::memory_order_relaxed);
        if (c == 0) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < c; ++i) sum += samples_[i];
        return sum / c;
    }

    inline double stdev() const noexcept {
        size_t c = count_.load(std::memory_order_relaxed);
        if (c == 0) return 0.0;
        double m = mean();
        double acc = 0.0;
        for (size_t i = 0; i < c; ++i) {
            double d = samples_[i] - m;
            acc += d * d;
        }
        return std::sqrt(acc / c);
    }

    inline void printLatest() const noexcept {
        double val = latest();
        std::printf("[RTTimer|%s] %.3f ms\n", name_.c_str(), val);
    }

    inline void printStatistics() const noexcept {
        double m = mean();
        double s = stdev();
        std::printf("[RTTimer|%s] mean=%.3f ms, std=%.3f ms\n", name_.c_str(), m, s);
    }

private:
    static constexpr size_t max_samples = 512;  // 고정 버퍼 (heap 없음)
    std::string name_;
    std::array<double, max_samples> samples_{};
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<size_t> index_;
    std::atomic<size_t> count_;
};
