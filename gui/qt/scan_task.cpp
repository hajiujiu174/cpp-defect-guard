#include "scan_task.hpp"

ScanTask::ScanTask(QObject* parent) : QObject(parent) {
    timer_.setInterval(25);
    connect(&timer_, &QTimer::timeout, this, [this]() { poll(); });
}
ScanTask::~ScanTask() {
    timer_.stop();
    if (busy()) { shared_->control->request_cancel(); future_.wait(); }
}
bool ScanTask::start(codeguard::fs::path root, codeguard::fs::path database, codeguard::ScanOptions options) {
    if (busy()) return false;
    root = codeguard::fs::absolute(root);
    database = codeguard::fs::absolute(database);
    if (!options.compile_commands.empty()) options.compile_commands = codeguard::utf8_path(
        codeguard::fs::absolute(codeguard::from_utf8(options.compile_commands)));
    auto shared = std::make_shared<Shared>();
    options.context.control = shared->control;
    const auto observer = options.context.progress;
    options.context.progress = [shared, observer](const codeguard::ScanProgress& value) {
        { std::lock_guard lock(shared->mutex); shared->progress = value; }
        if (observer) observer(value);
    };
    future_ = std::async(std::launch::async, [root, database, options, shared]() {
        TaskOutcome outcome;
        try {
            auto result = codeguard::import_project(root, database, options);
            if (!result.id) {
                outcome.error = "No snapshot saved. ";
                for (const auto& diagnostic : result.diagnostics) outcome.error += diagnostic.path + ": " + diagnostic.message + "\n";
                outcome.state = TaskState::failed;
            } else {
                outcome.state = result.analysis.status == "partial" || result.analysis.status == "failed"
                    ? TaskState::partial : TaskState::completed;
                outcome.result = std::move(result);
            }
        } catch (const codeguard::ScanCancelled&) {
            outcome.state = TaskState::cancelled;
        } catch (const std::exception& exception) {
            outcome.state = shared->control->state() == codeguard::ScanControl::State::cancel_requested
                ? TaskState::cancelled : TaskState::failed;
            outcome.error = exception.what();
        } catch (...) { outcome.error = "Unknown scan failure"; }
        // Seal failure/cancel races too: every accepted cancellation without a
        // committed result must finish as cancelled, even after import returned.
        if (shared->control->finish() == codeguard::ScanControl::State::cancel_requested && !outcome.result)
            outcome.state = TaskState::cancelled;
        return outcome;
    });
    shared_ = shared; state_ = TaskState::running; timer_.start();
    if (updated) updated(state_, {});
    return true;
}
bool ScanTask::cancel() {
    if (!busy() || !shared_->control->request_cancel()) return false;
    state_ = TaskState::cancelling;
    if (updated) {
        codeguard::ScanProgress progress;
        { std::lock_guard lock(shared_->mutex); progress = shared_->progress; }
        updated(state_, progress);
    }
    return true;
}
void ScanTask::poll() {
    if (!busy()) return;
    codeguard::ScanProgress progress;
    { std::lock_guard lock(shared_->mutex); progress = shared_->progress; }
    if (shared_->control->state() == codeguard::ScanControl::State::committing) state_ = TaskState::committing;
    if (updated) updated(state_, progress);
    if (future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    timer_.stop();
    const auto outcome = future_.get(); state_ = outcome.state;
    if (finished) finished(outcome);
}
