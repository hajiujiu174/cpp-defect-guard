#pragma once
#include "codeguard/engineering.hpp"
#include <QObject>
#include <QTimer>
#include <future>
#include <mutex>
#include <optional>
struct BuildOutcome { std::optional<codeguard::BuildRun> result; std::string error; };
class BuildTask final : public QObject {
    struct Shared {
        std::mutex mutex; std::string phase;
        std::shared_ptr<codeguard::ScanControl> control=std::make_shared<codeguard::ScanControl>();
    };
    QTimer timer_; std::shared_ptr<Shared> shared_; std::future<BuildOutcome> future_;
public:
    explicit BuildTask(QObject* parent=nullptr);
    ~BuildTask() override;
    bool start(std::filesystem::path root,std::filesystem::path database,codeguard::BuildOptions options);
    bool busy() const {return future_.valid();}
    bool cancel();
    std::function<void(const std::string&)> updated;
    std::function<void(const BuildOutcome&)> finished;
};
