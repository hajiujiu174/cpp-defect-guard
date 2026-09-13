#pragma once
#include "codeguard/report.hpp"
#include <QDialog>
#include <future>
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class ReportDialog final:public QDialog {
public:
    ReportDialog(const std::string& root,const codeguard::fs::path& database,QWidget* parent=nullptr);
    ~ReportDialog() override;
    void reject() override;
private:
    void load(bool reset);
    void exportSelected();
    void poll();
    void busy(bool value);
    void cancel();
    std::string root_;
    codeguard::fs::path database_;
    std::future<std::vector<codeguard::ScanSummary>> history_;
    std::future<codeguard::ReportFiles> export_;
    std::int64_t cursor_=0;
    bool more_=true;
    bool reset_pending_=false,close_pending_=false;
    std::shared_ptr<codeguard::ScanControl> control_;
    QComboBox *current_,*baseline_;
    QLineEdit* output_;
    QLabel* status_;
    QPushButton *refresh_,*older_,*generate_,*choose_,*close_,*cancel_;
};
