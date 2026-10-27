#pragma once
#include "codeguard/repair.hpp"
#include "codeguard/repair_comparison.hpp"
#include <QDialog>
#include <QTimer>
#include <future>
#include <mutex>
#include <map>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QCheckBox;
class QSpinBox;
class RepairDialog final : public QDialog {
public:
    RepairDialog(std::shared_ptr<const codeguard::ScanResult> snapshot,codeguard::Issue selected,
        std::filesystem::path database,codeguard::BuildOptions options,QWidget* parent=nullptr);
    ~RepairDialog() override;
    bool busy() const{return future_.valid();}
    void reject() override;
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    struct Shared {std::mutex mutex;std::string phase;std::shared_ptr<codeguard::ScanControl> control=std::make_shared<codeguard::ScanControl>();};
    struct Outcome {
        std::vector<codeguard::RepairCandidate> candidates;
        std::optional<codeguard::RepairValidation> validation;
        std::optional<codeguard::RepairBatch> batch;
        std::optional<codeguard::RepairCoverageComparison> comparison;
        std::filesystem::path exported;
        std::string error;
        bool cancelled=false;
    };
    void start(std::function<Outcome(const codeguard::ScanContext&)> work,bool cancellable=true,bool retain_pre_cancelled=false);
    void poll();
    void generate();
    void exportCandidate();
    void validateCandidate();
    void evaluateCandidates();
    void compareCandidate();
    void showValidation(const codeguard::RepairValidation& validation);
    void showBatch(const codeguard::RepairBatch& batch);
    void preview();
    void controls();
    void requestClose();
    QTimer timer_;
    std::shared_ptr<Shared> shared_;
    std::future<Outcome> future_;
    bool close_pending_=false,cancellable_=true;
    std::shared_ptr<const codeguard::ScanResult> snapshot_;
    codeguard::Issue selected_;
    std::filesystem::path database_;
    codeguard::BuildOptions options_;
    std::vector<codeguard::RepairCandidate> candidates_;
    std::map<std::string,codeguard::RepairValidation> validations_;
    QComboBox* choices_;
    QLabel *risk_,*status_;
    QLineEdit* output_;
    QLineEdit* gcov_;
    QCheckBox* coverage_;
    QCheckBox* per_test_;
    QSpinBox* test_limit_;
    QSpinBox* rounds_;
    QPushButton* compare_;
    QPlainTextEdit *diff_,*results_;
    QTableWidget* batch_table_;
    QPushButton *generate_,*export_,*validate_,*evaluate_,*cancel_;
};
