#include "report_dialog.hpp"
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString q(const std::string& s){return QString::fromUtf8(s.data(),static_cast<qsizetype>(s.size()));}
std::string s(const QString& q){const auto value=q.toUtf8();return {value.data(),static_cast<std::size_t>(value.size())};}
}
ReportDialog::ReportDialog(const std::string& root,const codeguard::fs::path& database,QWidget* parent):QDialog(parent),root_(root),database_(database){
    setObjectName("reportDialog");setWindowTitle(QStringLiteral("报告与版本对比"));resize(820,430);
    auto* layout=new QVBoxLayout(this);
    auto* explanation=new QLabel(QStringLiteral("选择保存的扫描版本，导出可独立查看的 HTML 和 JSON。对比会区分持续问题、抑制、覆盖变化与未再检出；未再检出仍需复核，不直接表示修复。"),this);explanation->setWordWrap(true);layout->addWidget(explanation);
    auto* form=new QFormLayout;layout->addLayout(form);
    current_=new QComboBox(this);current_->setObjectName("reportScan");current_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);current_->setMinimumContentsLength(36);
    baseline_=new QComboBox(this);baseline_->setObjectName("reportBaseline");baseline_->addItem(QStringLiteral("不对比，只导出当前版本"),qlonglong{0});baseline_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);baseline_->setMinimumContentsLength(36);
    form->addRow(QStringLiteral("报告版本"),current_);form->addRow(QStringLiteral("较早的基线"),baseline_);
    auto* pagination=new QHBoxLayout;refresh_=new QPushButton(QStringLiteral("载入最新记录"),this);older_=new QPushButton(QStringLiteral("载入更早记录"),this);older_->setObjectName("reportOlder");pagination->addWidget(refresh_);pagination->addWidget(older_);form->addRow(pagination);
    output_=new QLineEdit(this);output_->setObjectName("reportOutput");
    output_->setText(q(codeguard::utf8_path(database_.parent_path()/("report-"+s(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"))))));
    form->addRow(QStringLiteral("新报告目录"),output_);choose_=new QPushButton(QStringLiteral("选择报告的父目录"),this);form->addRow(choose_);
    auto* scope=new QLabel(QStringLiteral("目录必须位于源码目录之外，且尚不存在；已有报告不会被覆盖。报告保留扫描配置、解析诊断、问题证据和关联构建日志，导出期间在后台读取保存的数据。"),this);scope->setWordWrap(true);layout->addWidget(scope);
    status_=new QLabel(QStringLiteral("正在载入扫描历史…"),this);status_->setObjectName("reportStatus");status_->setWordWrap(true);status_->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(status_);layout->addStretch();
    auto* buttons=new QHBoxLayout;generate_=new QPushButton(QStringLiteral("导出 HTML 和 JSON"),this);generate_->setObjectName("reportGenerate");close_=new QPushButton(QStringLiteral("关闭"),this);buttons->addStretch();buttons->addWidget(generate_);buttons->addWidget(close_);layout->addLayout(buttons);
    connect(refresh_,&QPushButton::clicked,this,[this]{load(true);});connect(older_,&QPushButton::clicked,this,[this]{load(false);});connect(generate_,&QPushButton::clicked,this,[this]{exportSelected();});connect(close_,&QPushButton::clicked,this,&ReportDialog::reject);
    connect(choose_,&QPushButton::clicked,this,[this]{const auto parent=QFileDialog::getExistingDirectory(this,QStringLiteral("选择报告父目录"));if(!parent.isEmpty())output_->setText(parent+"/report-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));});
    auto* timer=new QTimer(this);timer->setInterval(25);connect(timer,&QTimer::timeout,this,[this]{poll();});timer->start();load(true);
}
ReportDialog::~ReportDialog(){if(history_.valid())history_.wait();if(export_.valid())export_.wait();}
void ReportDialog::busy(bool value){for(auto* button:{refresh_,generate_,choose_,close_})button->setEnabled(!value);older_->setEnabled(!value&&more_);current_->setEnabled(!value);baseline_->setEnabled(!value);output_->setEnabled(!value);if(!value)generate_->setEnabled(current_->count()>0);}
void ReportDialog::reject(){if(history_.valid()||export_.valid()){status_->setText(QStringLiteral("后台任务仍在执行，完成后可关闭。"));return;}QDialog::reject();}
void ReportDialog::load(bool reset){
    if(history_.valid()||export_.valid())return;
    if(reset){cursor_=0;current_->clear();baseline_->clear();baseline_->addItem(QStringLiteral("不对比，只导出当前版本"),qlonglong{0});}
    busy(true);status_->setText(QStringLiteral("正在载入扫描历史…"));
    history_=std::async(std::launch::async,[root=root_,database=database_,cursor=cursor_]{codeguard::SqliteDatabase db(database,true);return db.scans(root,cursor,50);});
}
void ReportDialog::exportSelected(){
    if(history_.valid()||export_.valid()||current_->currentIndex()<0)return;
    const auto id=current_->currentData().toLongLong(),baseline=baseline_->currentData().toLongLong();
    if(baseline>=id){status_->setText(QStringLiteral("基线必须早于报告版本，请重新选择。"));return;}
    const auto output=codeguard::from_utf8(s(output_->text()));busy(true);status_->setText(QStringLiteral("正在读取快照并生成报告…"));
    export_=std::async(std::launch::async,[root=root_,database=database_,id,baseline,output]{codeguard::SqliteDatabase db(database,true);auto current=db.snapshot(root,id,true);std::optional<codeguard::ScanResult> old;if(baseline)old=db.snapshot(root,baseline,true);return codeguard::export_report(current,output,old?&*old:nullptr);});
}
void ReportDialog::poll(){
    try{
        if(history_.valid()&&history_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
            const auto page=history_.get();more_=page.size()==50;
            for(const auto& scan:page){const auto label=QStringLiteral("#%1 · %2 · %3 · 问题 %4 / 抑制 %5").arg(scan.id).arg(q(codeguard::report_time(scan.scanned_at)),q(scan.analysis_status)).arg(scan.issue_count).arg(scan.suppressed_count);current_->addItem(label,static_cast<qlonglong>(scan.id));baseline_->addItem(label,static_cast<qlonglong>(scan.id));cursor_=scan.id;}
            busy(false);status_->setText(current_->count()?QStringLiteral("已载入 %1 个扫描版本。选择版本后导出。" ).arg(current_->count()):QStringLiteral("该工程没有保存的扫描记录。"));
        }
        if(export_.valid()&&export_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){const auto files=export_.get();busy(false);setProperty("reportExported",q(codeguard::utf8_path(files.html)));status_->setText(QStringLiteral("报告已保存：\n%1\n%2").arg(q(codeguard::utf8_path(files.html)),q(codeguard::utf8_path(files.json))));}
    }catch(const std::exception& e){busy(false);status_->setText(QStringLiteral("操作失败：")+q(e.what()));}
}
