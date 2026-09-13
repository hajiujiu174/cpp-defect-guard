#pragma once
#include "codeguard/scan_control.hpp"
#include <QObject>
#include <QTimer>
#include <future>
#include <optional>

template<class T> struct ReadOutcome {
    std::optional<T> result;
    std::string error;
    bool cancelled = false;
};

// GUI-thread owner; workers receive values and a cancellation context only.
// Acceptance is sealed at delivery, so cancelling a ready but undisplayed
// result also discards it. No database writes belong in this controller.
template<class T> class ReadTask final : public QObject {
public:
    explicit ReadTask(QObject* parent = nullptr) : QObject(parent) {
        timer_.setInterval(10);
        connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    }
    ~ReadTask() override {
        timer_.stop();
        if (busy()) { cancel(); future_.wait(); }
    }
    bool busy() const { return future_.valid(); }
    bool cancel() { return busy() && control_->request_cancel(); }
    template<class F> bool start(F work) {
        if (busy()) return false;
        auto control = std::make_shared<codeguard::ScanControl>();
        future_ = std::async(std::launch::async, [work = std::move(work), control]() mutable {
            ReadOutcome<T> outcome;
            try {
                codeguard::ScanContext context; context.control = control;
                context.check(); outcome.result = work(context); context.check();
            } catch (const codeguard::ScanCancelled&) { outcome.cancelled = true; }
            catch (const std::exception& e) { outcome.error = e.what(); }
            catch (...) { outcome.error = "Unknown background read failure"; }
            return outcome;
        });
        control_ = std::move(control); timer_.start(); return true;
    }
    std::function<void(ReadOutcome<T>)> finished;
private:
    void poll() {
        if (!busy() || future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        timer_.stop(); auto outcome = future_.get();
        if (control_->finish() == codeguard::ScanControl::State::cancel_requested) {
            outcome.result.reset(); outcome.error.clear(); outcome.cancelled = true;
        }
        if (finished) finished(std::move(outcome));
    }
    QTimer timer_;
    std::shared_ptr<codeguard::ScanControl> control_;
    std::future<ReadOutcome<T>> future_;
};
