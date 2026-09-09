#include "scan_task.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QLineEdit>
#include <QPushButton>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>

namespace cg = codeguard;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Gate {
    std::mutex mutex; std::condition_variable changed;
    bool entered = false, released = false;
    void wait() {
        std::unique_lock lock(mutex); entered = true;
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&]() { return released; })) throw std::runtime_error("test gate timeout");
    }
    bool ready() { std::lock_guard lock(mutex); return entered; }
    void release() { { std::lock_guard lock(mutex); released = true; } changed.notify_all(); }
};
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto dir = cg::fs::current_path() / ("qt-task-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!cg::fs::create_directory(dir)) return 1;
    int result = 0;
    try {
        const auto root = dir / "source", database = dir / "inventory.sqlite3";
        cg::fs::create_directory(root); std::ofstream(root / "a.cpp") << "int value;\n";
        const auto baseline = cg::import_project(root, database);
        auto visible_id = baseline.id;
        std::ofstream(root / "new.cpp") << "int other;\n";
        Gate gate; ScanTask task; QEventLoop loop; QTimer heartbeat, timeout;
        QLineEdit filter; QPushButton cancel;
        int live_ticks = 0, edited = 0, stage = 0, progress_events = 0;
        bool passed = false, released = false;
        const auto gui_thread = std::this_thread::get_id();
        QObject::connect(&filter, &QLineEdit::textChanged, [&]() { ++edited; });
        QObject::connect(&cancel, &QPushButton::clicked, [&]() { require(task.cancel(), "UI cancel not accepted"); });
        cg::ScanOptions options;
        options.context.progress = [&](const cg::ScanProgress& value) {
            require(std::this_thread::get_id() != gui_thread, "scan ran on GUI thread");
            if (value.phase == "before_commit") gate.wait();
        };
        auto fail = [&](const std::exception& error) {
            std::cerr << error.what() << '\n'; result = 1; gate.release(); task.cancel(); loop.quit();
        };
        task.updated = [&](TaskState, const cg::ScanProgress&) {
            require(std::this_thread::get_id() == gui_thread, "GUI progress callback on worker thread"); ++progress_events;
        };
        task.finished = [&](const TaskOutcome& outcome) {
            try {
                require(std::this_thread::get_id() == gui_thread && !task.busy(), "completion thread or busy state");
                if (outcome.result) visible_id = outcome.result->id;
                if (stage == 0) {
                    require(outcome.state == TaskState::cancelled && !outcome.result && visible_id == baseline.id, "cancel changed visible result");
                    cg::SqliteDatabase db(database, true);
                    require(db.latest(baseline.root).id == baseline.id && db.latest(baseline.root).files.size() == 1, "rollback failed");
                    require(live_ticks >= 2 && edited >= 1 && progress_events > 0, "UI did not process events during scan");
                    stage = 1;
                    require(task.start(root, database), "cannot restart after cancel");
                } else if (stage == 1) {
                    require(outcome.state == TaskState::completed && outcome.result->files.size() == 2 && visible_id > baseline.id, "rescan failed");
                    require(!task.cancel(), "late cancellation should be rejected");
                    stage = 2; require(task.start(root / "missing", database), "failed request not started");
                } else if (stage == 2) {
                    require(outcome.state == TaskState::failed && !outcome.result && visible_id > baseline.id, "failure replaced visible snapshot");
                    if (cg::clang_analysis_available()) {
                        std::ofstream commands(dir / "compile_commands.json");
                        commands << "[{\"directory\":\"" << cg::utf8_path(root)
                            << "\",\"file\":\"a.cpp\",\"arguments\":[\"clang++\",\"-c\",\"a.cpp\"]}]";
                        commands.close();
                        cg::ScanOptions partial; partial.compile_commands = cg::utf8_path(dir);
                        stage = 3; require(task.start(root, database, partial), "partial task not started");
                        return;
                    }
                    passed = true; loop.quit();
                } else {
                    require(outcome.state == TaskState::partial && outcome.result && outcome.result->analysis.status == "partial", "partial analysis mislabelled");
                    cg::SqliteDatabase db(database, true);
                    require(db.latest(baseline.root).id == visible_id && db.latest(baseline.root).analysis.status == "partial", "partial persistence mismatch");
                    passed = true; loop.quit();
                }
            } catch (const std::exception& error) { fail(error); }
        };
        QObject::connect(&heartbeat, &QTimer::timeout, [&]() {
            try {
                if (stage != 0 || !gate.ready() || released) return;
                ++live_ticks;
                require(task.busy() && !task.start(root, database), "duplicate start accepted");
                filter.setText(QString::number(live_ticks));
                if (live_ticks >= 2) {
                    cancel.click(); require(task.state() == TaskState::cancelling, "missing cancelling state");
                    gate.release(); released = true;
                }
            } catch (const std::exception& error) { fail(error); }
        });
        QObject::connect(&timeout, &QTimer::timeout, [&]() { gate.release(); task.cancel(); result = 1; loop.quit(); });
        require(task.start(root, database, options), "initial start failed");
        require(!task.start(root, database, options), "immediate duplicate accepted");
        heartbeat.start(2); timeout.setSingleShot(true); timeout.start(8000); loop.exec();
        require(passed, "async acceptance not completed");
        std::cout << "QT_BACKGROUND_OK responsive, duplicate rejected, rollback, restart, failure preserved\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::error_code error; cg::fs::remove_all(dir, error); return result;
}
