#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace codeguard {
class ScanCancelled final : public std::runtime_error {
public:
    ScanCancelled() : std::runtime_error("scan cancelled; no new snapshot committed") {}
};
// One control per scan. The commit fence linearizes cancel versus COMMIT.
class ScanControl {
public:
    enum class State { running, cancel_requested, committing, finished };
    bool request_cancel() {
        auto expected = State::running;
        return state_.compare_exchange_strong(expected, State::cancel_requested);
    }
    State state() const { return state_.load(); }
    void check() const { if (state() == State::cancel_requested) throw ScanCancelled(); }
    void begin_commit() {
        auto expected = State::running;
        if (!state_.compare_exchange_strong(expected, State::committing)) {
            check(); throw std::logic_error("scan control already consumed");
        }
    }
    State finish() { return state_.exchange(State::finished); }
private:
    std::atomic<State> state_{State::running};
};
struct ScanProgress {
    std::string phase, file;
    std::size_t completed = 0, total = 0; // total=0 means indeterminate
};
struct ScanContext {
    std::shared_ptr<ScanControl> control;
    // Called on the worker thread. Consumers must not touch GUI objects here.
    std::function<void(const ScanProgress&)> progress;
    void check() const { if (control) control->check(); }
    void report(const std::string& phase, const std::string& file = {}, std::size_t completed = 0, std::size_t total = 0) const {
        check();
        if (progress) progress({phase, file, completed, total});
        check();
    }
};
} // namespace codeguard
