#pragma once
#include "codeguard/application.hpp"
#include <QObject>
#include <QTimer>
#include <future>
#include <mutex>
#include <optional>

enum class TaskState { idle, running, cancelling, committing, completed, partial, cancelled, failed };
struct TaskOutcome {
    TaskState state = TaskState::failed;
    std::optional<codeguard::ScanResult> result;
    std::string error;
};
// GUI-thread controller. Only standard C++ values cross into the worker.
class ScanTask final : public QObject {
public:
    explicit ScanTask(QObject* parent = nullptr);
    ~ScanTask() override;
    bool start(codeguard::fs::path root, codeguard::fs::path database, codeguard::ScanOptions options = {});
    bool cancel();
    bool busy() const { return future_.valid(); }
    TaskState state() const { return state_; }
    std::function<void(TaskState, const codeguard::ScanProgress&)> updated;
    std::function<void(const TaskOutcome&)> finished;
private:
    struct Shared {
        std::mutex mutex;
        codeguard::ScanProgress progress;
        std::shared_ptr<codeguard::ScanControl> control = std::make_shared<codeguard::ScanControl>();
    };
    void poll();
    QTimer timer_;
    TaskState state_ = TaskState::idle;
    std::shared_ptr<Shared> shared_;
    std::future<TaskOutcome> future_;
};
