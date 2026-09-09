#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>

namespace codeguard {
// Bounded producer queue; packaged_task propagates exceptions to each caller.
// Destruction drains accepted work and joins every worker.
class ThreadPool {
    std::mutex mutex_;
    std::condition_variable ready_, space_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::size_t capacity_;
    bool closing_ = false;
public:
    ThreadPool(std::size_t threads, std::size_t capacity) : capacity_(capacity) {
        if (!threads || threads > 64 || !capacity) throw std::invalid_argument("invalid pool size/capacity");
        try {
            for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                        if (queue_.empty()) return;
                        task = std::move(queue_.front()); queue_.pop_front(); space_.notify_one();
                    }
                    task();
                }
            });
        } catch (...) { close(); throw; }
    }
    ThreadPool(const ThreadPool&) = delete;
    ~ThreadPool() { close(); }
    template<class F> auto submit(F&& function) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(function));
        auto result = task->get_future();
        std::unique_lock lock(mutex_);
        space_.wait(lock, [&] { return closing_ || queue_.size() < capacity_; });
        if (closing_) throw std::runtime_error("thread pool closed");
        queue_.emplace_back([task] { (*task)(); }); ready_.notify_one();
        return result;
    }
    void close() noexcept {
        { std::lock_guard lock(mutex_); closing_ = true; }
        ready_.notify_all(); space_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
};
} // namespace codeguard
