#include "repair_set_dialog.hpp"
#include "repair_test_coverage_ui.hpp"
#include "repair_comparison_ui.hpp"
#include <QSpinBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cg=codeguard;
namespace {
QString q(const std::string& text){return QString::fromUtf8(text.data(),static_cast<qsizetype>(text.size()));}
std::string u(const QString& text){return text.toUtf8().toStdString();}
QString recipe(const std::string& name){return name=="condition.assignment_result"?QStringLiteral("先赋值再比较"):name=="condition.compare"?QStringLiteral("改为相等比较"):name=="condition.intent"?QStringLiteral("保留赋值并加括号"):QStringLiteral("按局部数组容量限制输出");}
QString risk(const std::string& name){return name=="condition.assignment_result"?QStringLiteral("改变保存值、类型转换或求值顺序，需核对错误处理"):name=="condition.compare"?QStringLiteral("停止赋值并改变分支行为，需确认意图"):name=="condition.intent"?QStringLiteral("仅消除意图提示，行为缺陷可能保留"):QStringLiteral("可能截断，需核对返回值和错误处理");}
}
RepairSetDialog::RepairSetDialog(std::shared_ptr<const cg::ScanResult> snapshot,std::vector<cg::Issue> issues,
    cg::fs::path database,cg::BuildOptions options,QWidget* parent):QDialog(parent),snapshot_(std::move(snapshot)),issues_(std::move(issues)),database_(std::move(database)),options_(std::move(options)){
    setObjectName("repairSetDialog");setWindowTitle(QStringLiteral("组合修复候选 / 差异与副本验证"));setModal(true);resize(1060,860);
    auto* outer=new QVBoxLayout(this);auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);auto* content=new QWidget(scroll);scroll->setWidget(content);outer->addWidget(scroll);
    auto* layout=new QVBoxLayout(content);layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* intro=new QLabel(QStringLiteral("从选中问题的候选中明确勾选 2—16 项。每个问题只能选一个方案，重叠范围会被拒绝。组合在同一份副本中构建和测试；原始工程不会自动写回，正确性仍需人工审阅。"),content);intro->setWordWrap(true);layout->addWidget(intro);
    candidates_table_=new QTableWidget(0,4,content);candidates_table_->setObjectName("setCandidates");candidates_table_->setHorizontalHeaderLabels({QStringLiteral("选用"),QStringLiteral("问题位置"),QStringLiteral("候选方式"),QStringLiteral("影响")});
    candidates_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);candidates_table_->setSelectionBehavior(QAbstractItemView::SelectRows);candidates_table_->setMinimumHeight(160);candidates_table_->setMaximumHeight(230);candidates_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);candidates_table_->horizontalHeader()->setStretchLastSection(true);layout->addWidget(candidates_table_);
    diff_=new QPlainTextEdit(content);diff_->setObjectName("setDiff");diff_->setReadOnly(true);diff_->setLineWrapMode(QPlainTextEdit::NoWrap);diff_->setMinimumHeight(170);diff_->setStyleSheet("font-family: Consolas; font-size: 13px;");layout->addWidget(diff_,1);
    output_=new QLineEdit(content);output_->setObjectName("setOutput");output_->setText(q(cg::utf8_path(options_.output_directory.empty()?database_.parent_path()/"repair-sets":options_.output_directory)));output_->setPlaceholderText(QStringLiteral("记录目录，必须位于原始源码树外"));layout->addWidget(output_);
    auto* coverage_row=new QHBoxLayout;coverage_=new QCheckBox(QStringLiteral("采集 gcov（GNU 编译器 / ASCII 输出目录）"),content);coverage_->setObjectName("setCoverage");coverage_->setChecked(options_.collect_coverage);gcov_=new QLineEdit(q(options_.gcov),content);gcov_->setObjectName("setGcov");coverage_row->addWidget(coverage_);coverage_row->addWidget(gcov_,1);layout->addLayout(coverage_row);
    auto* replay_row=new QHBoxLayout;per_test_=new QCheckBox(QStringLiteral("逐测试组重跑（含依赖 / fixture，CTest 3.29+）"),content);per_test_->setObjectName("setPerTest");per_test_->setChecked(options_.collect_test_coverage);test_limit_=new QSpinBox(content);test_limit_->setObjectName("setTestLimit");test_limit_->setRange(1,256);test_limit_->setValue(static_cast<int>(options_.coverage_test_limit));replay_row->addWidget(per_test_);replay_row->addWidget(new QLabel(QStringLiteral("声明测试数上限"),content));replay_row->addWidget(test_limit_);replay_row->addStretch();layout->addLayout(replay_row);connect(per_test_,&QCheckBox::toggled,this,[this]{controls();});
    auto* buttons=new QHBoxLayout;generate_=new QPushButton(QStringLiteral("重新生成"),content);generate_->setObjectName("setGenerate");export_=new QPushButton(QStringLiteral("导出组合"),content);export_->setObjectName("setExport");validate_=new QPushButton(QStringLiteral("验证组合副本"),content);validate_->setObjectName("setValidate");cancel_=new QPushButton(QStringLiteral("取消任务"),content);cancel_->setObjectName("setCancel");auto* close=new QPushButton(QStringLiteral("关闭"),content);close->setObjectName("setClose");for(auto* button:{generate_,export_,validate_,cancel_,close})buttons->addWidget(button);layout->addLayout(buttons);
    status_=new QLabel(QStringLiteral("正在准备候选…"),content);status_->setObjectName("setStatus");status_->setWordWrap(true);layout->addWidget(status_);
    auto* compare_row=new QHBoxLayout;compare_=new QPushButton(QStringLiteral("原版 / 组合多轮覆盖对照"),content);compare_->setObjectName("setCompare");rounds_=new QSpinBox(content);rounds_->setObjectName("setRounds");rounds_->setRange(2,5);rounds_->setValue(2);compare_row->addWidget(compare_);compare_row->addWidget(new QLabel(QStringLiteral("每版轮数（独立 GNU 构建）"),content));compare_row->addWidget(rounds_);compare_row->addStretch();layout->addLayout(compare_row);connect(compare_,&QPushButton::clicked,this,[this]{compareSet();});
    result_=new QPlainTextEdit(content);result_->setObjectName("setResults");result_->setReadOnly(true);result_->setMinimumHeight(155);result_->hide();layout->addWidget(result_);
    connect(candidates_table_,&QTableWidget::itemChanged,this,[this]{if(!busy())compose();});connect(coverage_,&QCheckBox::toggled,this,[this]{controls();});
    connect(generate_,&QPushButton::clicked,this,[this]{generate();});connect(export_,&QPushButton::clicked,this,[this]{exportSet();});connect(validate_,&QPushButton::clicked,this,[this]{validateSet();});connect(close,&QPushButton::clicked,this,[this]{requestClose();});
    connect(cancel_,&QPushButton::clicked,this,[this]{if(shared_&&shared_->control->request_cancel()){status_->setText(QStringLiteral("正在取消并保留已产生的记录…"));cancel_->setEnabled(false);}});
    timer_.setInterval(20);connect(&timer_,&QTimer::timeout,this,[this]{poll();});controls();QTimer::singleShot(0,this,[this]{generate();});
}
RepairSetDialog::~RepairSetDialog(){timer_.stop();if(busy()){shared_->control->request_cancel();future_.wait();}}
void RepairSetDialog::controls(){generate_->setEnabled(!busy());candidates_table_->setEnabled(!busy());output_->setEnabled(!busy());coverage_->setEnabled(!busy());gcov_->setEnabled(!busy()&&coverage_->isChecked());per_test_->setEnabled(!busy()&&coverage_->isChecked());test_limit_->setEnabled(!busy()&&coverage_->isChecked()&&per_test_->isChecked());export_->setEnabled(!busy()&&set_.has_value());validate_->setEnabled(!busy()&&set_.has_value());compare_->setEnabled(!busy()&&set_.has_value());rounds_->setEnabled(!busy());cancel_->setEnabled(busy()&&cancellable_);}
void RepairSetDialog::start(std::function<Outcome(const cg::ScanContext&)> work,bool cancellable){
    if(busy())return;shared_=std::make_shared<Shared>();cancellable_=cancellable;const auto shared=shared_;
    future_=std::async(std::launch::async,[work=std::move(work),shared]{Outcome out;cg::ScanContext context;context.control=shared->control;context.progress=[shared](const cg::ScanProgress& p){std::lock_guard lock(shared->mutex);shared->phase=p.phase;};
        try{out=work(context);}catch(const cg::ScanCancelled&){out.cancelled=true;}catch(const std::exception& e){out.error=e.what();}
        if(shared->control->finish()==cg::ScanControl::State::cancel_requested&&!out.validation&&!out.comparison&&out.exported.empty()){out.cancelled=true;out.candidates.clear();}return out;});controls();timer_.start();
}
void RepairSetDialog::generate(){
    if(busy())return;set_.reset();candidates_.clear();{const QSignalBlocker block(candidates_table_);candidates_table_->setRowCount(0);}diff_->clear();result_->clear();result_->hide();status_->setText(QStringLiteral("正在一次 AST 解析中生成所选问题的候选…"));const auto snapshot=snapshot_;const auto issues=issues_;
    start([snapshot,issues](const cg::ScanContext& context){Outcome out;out.candidates=cg::propose_repairs(*snapshot,issues,context);return out;});
}
void RepairSetDialog::compose(){
    set_.reset();diff_->clear();result_->clear();result_->hide();try{
        std::size_t count=0;for(int n=0;n<candidates_table_->rowCount();++n)if(candidates_table_->item(n,0)->checkState()==Qt::Checked)++count;
        if(count<2||count>16){status_->setText(QStringLiteral("请明确勾选 2—16 项；每个问题只选一个候选。"));controls();return;}
        std::vector<cg::RepairCandidate> selected;for(int n=0;n<candidates_table_->rowCount();++n)if(candidates_table_->item(n,0)->checkState()==Qt::Checked)selected.push_back(candidates_.at(static_cast<std::size_t>(n)));
        auto set=cg::compose_repairs(std::move(selected));auto diff=cg::repair_set_diff(set);const bool truncated=diff.size()>512000;
        if(truncated){const auto end=diff.rfind('\n',512000);diff.resize(end==std::string::npos?512000:end);}
        diff_->setPlainText(q(diff)+(truncated?QStringLiteral("\n预览已截断；导出文件包含完整差异。\n"):QString{}));
        status_->setText(QStringLiteral("已组合 %1 项，涉及 %2 个文件。请审阅差异和影响，再导出或验证副本。").arg(set.components.size()).arg(cg::repair_set_changes(set).size()));set_=std::move(set);
    }catch(const std::exception& e){status_->setText(QStringLiteral("组合被拒绝：")+q(e.what()));}controls();
}
void RepairSetDialog::exportSet(){if(busy()||!set_)return;const auto set=*set_;const auto output=cg::from_utf8(u(output_->text()));status_->setText(QStringLiteral("正在导出组合、来源和差异…"));start([set,output](const cg::ScanContext&){Outcome out;out.exported=cg::export_repair_set(set,output);return out;},false);}
void RepairSetDialog::validateSet(){
    if(busy()||!set_)return;const auto set=*set_;auto options=options_;options.output_directory=cg::from_utf8(u(output_->text()));options.collect_coverage=coverage_->isChecked();options.collect_test_coverage=options.collect_coverage&&per_test_->isChecked();options.coverage_test_limit=static_cast<unsigned>(test_limit_->value());options.gcov=u(gcov_->text());const auto root=cg::from_utf8(snapshot_->root),database=database_;status_->setText(QStringLiteral("正在验证同一份组合副本，逐项核对目标…"));
    start([set,options,root,database](const cg::ScanContext& context)mutable{Outcome out;options.control=context.control;options.progress=[context](const std::string& phase){context.report(phase);};out.validation=cg::validate_repair_set(root,database,set,options);return out;});
}
void RepairSetDialog::compareSet(){
    if(busy()||!set_)return;const auto parts=set_->components;auto options=options_;options.output_directory=cg::from_utf8(u(output_->text()));options.gcov=u(gcov_->text());options.coverage_test_limit=static_cast<unsigned>(test_limit_->value());const auto rounds=static_cast<unsigned>(rounds_->value());const auto root=cg::from_utf8(snapshot_->root),database=database_;result_->clear();result_->hide();status_->setText(QStringLiteral("正在多轮独立构建原版与组合并逐组采集覆盖记录…"));
    start([parts,options,root,database,rounds](const cg::ScanContext& context)mutable{Outcome out;options.control=context.control;options.progress=[context](const std::string& phase){context.report(phase);};out.comparison=cg::compare_repair_coverage(root,database,parts,options,rounds);return out;});
}
void RepairSetDialog::poll(){
    if(!busy())return;if(future_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){if(shared_->control->state()!=cg::ScanControl::State::cancel_requested){std::string phase;{std::lock_guard lock(shared_->mutex);phase=shared_->phase;}
            const std::map<std::string,QString> names{{"repair_ast",QStringLiteral("重新解析 AST")},{"prepare",QStringLiteral("核对并复制源码")},{"repair_baseline_copy",QStringLiteral("保留未修改的对照副本")},{"configure",QStringLiteral("配置组合副本")},{"build",QStringLiteral("编译组合副本")},{"test",QStringLiteral("运行组合测试")},{"repair_coverage",QStringLiteral("采集覆盖率")},{"repair_test_coverage",QStringLiteral("逐测试组重跑与独立采集")},{"repair_analysis",QStringLiteral("静态复查全部目标")}};
            if(const auto found=names.find(phase);found!=names.end())status_->setText(found->second+QStringLiteral("…"));}return;}
    timer_.stop();auto out=future_.get();
    if(out.comparison){result_->setPlainText(repairComparisonText(*out.comparison));result_->show();status_->setText(out.comparison->status=="cancelled"?QStringLiteral("多轮对照已取消，全部轮次状态保留"):out.comparison->ready_for_review?QStringLiteral("组合多轮对照完成；正确性仍需人工审阅"):QStringLiteral("多轮对照未通过全部门槛：")+q(out.comparison->status));}
    else if(out.validation){const auto& r=*out.validation;QString text=QStringLiteral("状态：%1\n静态复查：%2；原始对照：%3\n原始 C/C++ 源码：%4\n修复正确性：未评估\n").arg(q(r.status),q(r.analysis_status),q(r.baseline_status),r.build.source_unchanged?QStringLiteral("内容一致"):QStringLiteral("未确认"));
        for(const auto& step:r.build.steps)if(step.name=="build"||step.name=="test")text+=q(step.name)+": "+q(step.result.status)+(step.name=="test"?QStringLiteral("（总/失败/跳过 %1/%2/%3；-1 未知）").arg(step.tests_total).arg(step.tests_failed).arg(step.tests_skipped):QString{})+'\n';
        for(const auto& target:r.targets)text+=QStringLiteral("%1 %2:%3:%4 → %5:%6：原问题复现 %7，目标未再检出 %8，行执行记录 %9\n").arg(q(target.rule_id),q(target.file)).arg(target.line).arg(target.column).arg(target.revised_line).arg(target.revised_column).arg(target.baseline_reproduced?QStringLiteral("是"):QStringLiteral("未确认"),target.target_not_detected?QStringLiteral("是"):QStringLiteral("未确认"),q(target.coverage_line_status));
        text+=QStringLiteral("新增/重新显现告警：%1；覆盖率采集：%2\n记录：%3\n%4").arg(r.new_or_unmasked_issues<0?QStringLiteral("未知"):QString::number(r.new_or_unmasked_issues),q(r.coverage.status),q(r.workspace),q(r.error));text+='\n'+repairTestCoverageText(r.test_coverage);result_->setPlainText(text);result_->show();status_->setText(r.status=="cancelled"?QStringLiteral("组合验证已取消，全部目标记录保留"):r.gates_passed?QStringLiteral("组合通过本次门槛；仍需人工审阅正确性"):QStringLiteral("组合未通过门槛：")+q(r.status));
    }else if(!out.exported.empty())status_->setText(QStringLiteral("已导出组合：")+q(cg::utf8_path(out.exported)));
    else if(out.cancelled)status_->setText(QStringLiteral("组合任务已取消"));
    else if(!out.error.empty())status_->setText(QStringLiteral("组合任务失败：")+q(out.error));
    else{candidates_=std::move(out.candidates);{const QSignalBlocker block(candidates_table_);candidates_table_->setRowCount(static_cast<int>(candidates_.size()));for(std::size_t n=0;n<candidates_.size();++n){const auto& c=candidates_[n];auto* check=new QTableWidgetItem;check->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable);check->setCheckState(Qt::Unchecked);check->setData(Qt::UserRole,q(c.edit.recipe));check->setData(Qt::UserRole+1,q(c.edit.file));check->setData(Qt::UserRole+2,c.edit.line);candidates_table_->setItem(static_cast<int>(n),0,check);candidates_table_->setItem(static_cast<int>(n),1,new QTableWidgetItem(q(c.edit.rule_id)+" "+q(c.edit.file)+":"+QString::number(c.edit.line)+":"+QString::number(c.edit.column)));candidates_table_->setItem(static_cast<int>(n),2,new QTableWidgetItem(recipe(c.edit.recipe)));auto* impact=new QTableWidgetItem(risk(c.edit.recipe));impact->setToolTip(q(c.edit.risk));candidates_table_->setItem(static_cast<int>(n),3,impact);}}compose();if(candidates_.empty())status_->setText(QStringLiteral("所选问题没有可用的确定性候选"));}
    controls();if(close_pending_)QTimer::singleShot(0,this,[this]{QDialog::reject();});
}
void RepairSetDialog::requestClose(){if(!busy()){QDialog::reject();return;}close_pending_=true;shared_->control->request_cancel();status_->setText(QStringLiteral("等待组合任务安全结束并保留记录…"));}
void RepairSetDialog::reject(){requestClose();}
void RepairSetDialog::closeEvent(QCloseEvent* event){if(busy()){requestClose();event->ignore();}else QDialog::closeEvent(event);}
