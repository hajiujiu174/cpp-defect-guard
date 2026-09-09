#include "scan_task.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QProcess>
#include <sqlite3.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>

namespace cg = codeguard;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct WriterLock {
    sqlite3* db = nullptr;
    explicit WriterLock(const cg::fs::path& path, bool exclusive) {
        if (sqlite3_open_v2(cg::utf8_path(path).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
            sqlite3_exec(db, exclusive ? "BEGIN EXCLUSIVE" : "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            throw std::runtime_error("cannot hold writer lock");
        }
    }
    ~WriterLock() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(db); }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        require(argc >= 2 && argc <= 4, "usage: qt-scale-tests inventory|clang|lock|read_lock|window [count] [gui executable]");
        const std::string mode = argv[1];
        require(mode == "inventory" || mode == "clang" || mode == "lock" || mode == "read_lock" || mode == "window", "unknown mode");
        const bool window = mode == "window", clang = mode == "clang" || window;
        const bool exclusive = mode == "read_lock", locked = mode == "lock" || exclusive;
        const int count = locked ? 1 : (argc >= 3 ? std::stoi(argv[2]) : (clang ? 128 : 2048));
        require(count >= (locked ? 1 : 64) && count <= (clang ? 1024 : 10000), "fixture count out of bounds");
        require(!clang || cg::clang_analysis_available(), "Clang is required for this mode");
        QTemporaryDir temporary(QString::fromStdString(cg::utf8_path(cg::fs::current_path() / "qt-scale-XXXXXX")));
        require(temporary.isValid(), "temporary workspace unavailable");
        const auto dir = cg::from_utf8(temporary.path().toUtf8().toStdString());
        const auto root = dir / "source", database = dir / "inventory.sqlite3";
        cg::fs::create_directory(root);
        std::ofstream commands(dir / "compile_commands.json"); commands << '[';
        for (int i = 0; i < count; ++i) {
            const auto file = "unit_" + std::to_string(i) + ".cpp";
            std::ofstream source(root / file);
            // Independent, real translation units: 32 branch-bearing functions each.
            for (int j = 0; j < 32; ++j)
                source << "int function_" << i << '_' << j << "(int n) { if (n > " << j << ") return n - " << j << "; return n + 1; }\n";
            require(source.good(), "fixture source write failed");
            if (i) commands << ',';
            commands << "{\"directory\":\"" << cg::utf8_path(root) << "\",\"file\":\"" << file
                << "\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\"" << file << "\"]}";
        }
        commands << ']'; commands.close(); require(commands.good(), "fixture commands write failed");
        const auto before = cg::scan_project(root);
        const auto baseline = cg::import_project(root, database);
        if (window) {
            require(argc == 4, "window mode requires GUI executable path");
            QProcess child;
            child.setProgram(QString::fromLocal8Bit(argv[3]));
            child.setArguments({"--smoke-test", QString::fromStdString(cg::utf8_path(root)), "--database",
                QString::fromStdString(cg::utf8_path(database)), "--compile-commands", temporary.path()});
            child.start(); require(child.waitForStarted(5000), "GUI failed to start");
            QEventLoop child_loop; QTimer deadline;
            QObject::connect(&child, &QProcess::finished, &child_loop, &QEventLoop::quit);
            QObject::connect(&deadline, &QTimer::timeout, [&]() { child.kill(); child_loop.quit(); });
            deadline.setSingleShot(true); deadline.start(120000);
            if (child.state() != QProcess::NotRunning) child_loop.exec();
            if (child.state() != QProcess::NotRunning) child.waitForFinished(5000);
            const auto output = child.readAllStandardOutput();
            std::cout << output.constData(); std::cerr << child.readAllStandardError().constData();
            require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 && output.contains("GUI_SMOKE_OK"), "large window acceptance failed");
            const auto after = cg::scan_project(root);
            require(before.files.size() == after.files.size(), "window changed source inventory");
            for (std::size_t i = 0; i < before.files.size(); ++i)
                require(before.files[i].hash == after.files[i].hash, "window changed source bytes");
            std::cout << "QT_SCALE_WINDOW_OK files=" << count << " functions=" << count * 32 << '\n';
            return 0;
        }
        std::unique_ptr<WriterLock> lock;
        if (locked) lock = std::make_unique<WriterLock>(database, exclusive);

        ScanTask task; QEventLoop loop; QTimer heartbeat, timeout; QLineEdit filter;
        QElapsedTimer clock; clock.start();
        cg::ScanOptions options; if (clang) options.compile_commands = cg::utf8_path(dir);
        std::int64_t visible_id = baseline.id;
        qint64 last_tick = 0, start_ms = 0, cancel_ms = -1;
        int stage = locked ? 1 : 0, ticks = 0, progress_events = 0;
        bool passed = false, cancel_scheduled = false;
        std::vector<qint64> gaps;
        QJsonObject report{{"mode", QString::fromStdString(mode)}, {"files", count}, {"functions_per_file", 32}};
        auto fail = [&](const std::exception& error) {
            std::cerr << error.what() << '\n'; task.cancel(); loop.quit();
        };
        task.updated = [&](TaskState state, const cg::ScanProgress& value) {
            try {
                ++progress_events;
                if (stage != 1 || cancel_scheduled || state != TaskState::running) return;
                if ((locked && value.phase == (exclusive ? "scanning" : "saving")) || (!locked && value.phase == (clang ? "analyzing" : "scanning") && value.completed > 0)) {
                    cancel_scheduled = true;
                    QTimer::singleShot(locked ? 100 : 0, &task, [&]() {
                        try { cancel_ms = clock.elapsed(); require(task.cancel(), "scale cancel was not accepted"); }
                        catch (const std::exception& error) { fail(error); }
                    });
                }
            } catch (const std::exception& error) { fail(error); }
        };
        task.finished = [&](const TaskOutcome& outcome) {
            try {
                const auto now = clock.elapsed(); gaps.push_back(now - last_tick);
                require(ticks >= 2 && filter.text() == QString::number(ticks), "event loop did not handle input during work");
                require(*std::max_element(gaps.begin(), gaps.end()) < 500, "background task stalled GUI event loop >= 500 ms");
                if (stage == 0) {
                    require(outcome.state == TaskState::completed && outcome.result && outcome.result->files.size() == static_cast<std::size_t>(count), "scale scan failed");
                    if (clang) require(outcome.result->analysis.metrics.size() == static_cast<std::size_t>(count * 32), "scale Clang coverage mismatch");
                    visible_id = outcome.result->id;
                    report["scan_ms"] = static_cast<double>(now - start_ms);
                    report["scan_ui_ticks"] = ticks;
                    report["scan_max_ui_gap_ms"] = static_cast<double>(*std::max_element(gaps.begin(), gaps.end()));
                    stage = 1; gaps.clear(); ticks = 0; start_ms = last_tick = clock.elapsed();
                    require(task.start(root, database, options), "scale restart failed");
                    return;
                }
                require(cancel_ms >= 0 && outcome.state == TaskState::cancelled && !outcome.result, "scale cancellation missed");
                report["cancel_ms"] = static_cast<double>(now - cancel_ms);
                report["cancel_ui_ticks"] = ticks;
                report["cancel_max_ui_gap_ms"] = static_cast<double>(*std::max_element(gaps.begin(), gaps.end()));
                report["progress_callbacks"] = progress_events;
                lock.reset();
                cg::SqliteDatabase db(database, true);
                require(db.latest(baseline.root).id == visible_id, "cancel replaced persisted snapshot");
                std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
                if (locked) require(now - cancel_ms < 500, "database lock made cancellation wait >= 500 ms");
                passed = true; loop.quit();
            } catch (const std::exception& error) { fail(error); }
        };
        QObject::connect(&heartbeat, &QTimer::timeout, [&]() {
            try {
                const auto now = clock.elapsed(); gaps.push_back(now - last_tick); last_tick = now;
                filter.setText(QString::number(++ticks));
                require(!task.busy() || !task.start(root, database, options), "duplicate scale scan accepted");
            } catch (const std::exception& error) { fail(error); }
        });
        QObject::connect(&timeout, &QTimer::timeout, [&]() { task.cancel(); std::cerr << "scale timeout\n"; loop.quit(); });
        require(task.start(root, database, options), "scale start failed");
        heartbeat.start(10); timeout.setSingleShot(true); timeout.start(120000); loop.exec();
        require(passed, "scale acceptance failed");
        const auto after = cg::scan_project(root);
        require(before.files.size() == after.files.size(), "scale test changed sources");
        for (std::size_t i = 0; i < before.files.size(); ++i)
            require(before.files[i].path == after.files[i].path && before.files[i].hash == after.files[i].hash, "scale test modified source bytes");
        std::cout << "QT_SCALE_OK source unchanged, responsive, duplicate rejected, cancellation preserved snapshot\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
