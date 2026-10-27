#pragma once
#include "codeguard/repair.hpp"
#include "codeguard/repair_comparison.hpp"
#include <QDialog>
#include <QTimer>
#include <future>
#include <mutex>

class QTableWidget;
class QPlainTextEdit;
class QLabel;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QPushButton;
class RepairSetDialog final : public QDialog {
public:
    RepairSetDialog(std::shared_ptr<const codeguard::ScanResult> snapshot,std::vector<codeguard::Issue> issues,
        std::filesystem::path database,codeguard::BuildOptions options,QWidget* parent=nullptr);
    ~RepairSetDialog() override;
    bool busy() const{return future_.valid();}
    void reject() override;
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    struct Shared {std::mutex mutex;std::string phase;std::shared_ptr<codeguard::ScanControl> control=std::make_shared<codeguard::ScanControl>();};
    struct Outcome {std::vector<codeguard::RepairCandidate> candidates;std::optional<codeguard::RepairValidation> validation;std::optional<codeguard::RepairCoverageComparison> comparison;std::filesystem::path exported;std::string error;bool cancelled=false;};
    void start(std::function<Outcome(const codeguard::ScanContext&)> work,bool cancellable=true);
    void generate();
    void compose();
    void exportSet();
    void validateSet();
    void compareSet();
    void poll();
    void controls();
    void requestClose();
    std::shared_ptr<const codeguard::ScanResult> snapshot_;
    std::vector<codeguard::Issue> issues_;
    std::filesystem::path database_;
    codeguard::BuildOptions options_;
    std::vector<codeguard::RepairCandidate> candidates_;
    std::optional<codeguard::RepairSet> set_;
    std::future<Outcome> future_;
    std::shared_ptr<Shared> shared_;
    QTimer timer_;
    bool close_pending_=false,cancellable_=true;
    QTableWidget* candidates_table_;
    QPlainTextEdit *diff_,*result_;
    QLabel* status_;
    QLineEdit *output_,*gcov_;
    QCheckBox* coverage_;
    QCheckBox* per_test_;
    QSpinBox* test_limit_;
    QSpinBox* rounds_;
    QPushButton* compare_;
    QPushButton *generate_,*export_,*validate_,*cancel_;
};
