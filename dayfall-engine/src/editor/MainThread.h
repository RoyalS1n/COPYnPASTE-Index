#pragma once
// Work posted from MCP transport threads, run on the main thread between frames.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>

namespace df {
class MainThreadQueue {
public:
    std::future<void> post(std::function<void()> fn) {
        std::packaged_task<void()> task(std::move(fn));
        auto f = task.get_future();
        {
            std::lock_guard lock(m_);
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
        return f;
    }
    // runs queued work; returns how many items ran
    size_t drain(size_t max = 16) {
        size_t n = 0;
        while (n < max) {
            std::packaged_task<void()> t;
            {
                std::lock_guard lock(m_);
                if (q_.empty()) break;
                t = std::move(q_.front());
                q_.pop_front();
            }
            t();
            ++n;
        }
        return n;
    }
    size_t waitAndDrain(std::chrono::milliseconds timeout) {
        {
            std::unique_lock lock(m_);
            cv_.wait_for(lock, timeout, [&] { return !q_.empty(); });
        }
        return drain();
    }
    void wake() { cv_.notify_all(); }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::packaged_task<void()>> q_;
};
}  // namespace df
