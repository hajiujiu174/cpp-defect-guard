#include "repair_dialog.hpp"
#include "repair_test_coverage_ui.hpp"
#include "repair_comparison_ui.hpp"
#include <QSpinBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QFileInfo>
#include <QLabel>
#include <QHeaderView>
#include <QTableWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace cg=codeguard;
namespace {
QString q(const std::string& s){return QString::fromUtf8(s.data(),static_cast<int>(s.size()));}
std::string u(const QString& s){return s.toUtf8().toStdString();}
QString recipeName(const std::string& name){if(name=="condition.assignment_result")return QStringLiteral("先保存返回值，再比较");if(name=="condition.compare")return QStringLiteral("将条件赋值改为相等比较");if(name=="condition.intent")return QStringLiteral("为有意赋值加括号");return QStringLiteral("按局部数组容量限制格式化输出");}
QString state(const std::string& s){if(s=="passed")return QStringLiteral("通过");if(s=="failed")return QStringLiteral("失败");if(s=="cancelled")return QStringLiteral("取消");if(s=="skipped"||s=="not_run")return QStringLiteral("未运行");return q(s);}
}
RepairDialog::RepairDialog(std::shared_ptr<const cg::ScanResult> snapshot,cg::Issue selected,cg::fs::path database,cg::BuildOptions options,QWidget* parent)
    :QDialog(parent),snapshot_(std::move(snapshot)),selected_(std::move(selected)),database_(std::move(database)),options_(std::move(options)) {
    setObjectName("repairDialog");setWindowTitle(QStringLiteral("修复候选 · 差异预览与副本验证"));setWindowModality(Qt::WindowModal);resize(980,780);setMinimumSize(720,560);
    auto* outer=new QVBoxLayout(this);auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto* content=new QWidget(scroll);scroll->setWidget(content);outer->addWidget(scroll);
    auto* layout=new QVBoxLayout(content);layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* intro=new QLabel(QStringLiteral("为选中的活动问题生成候选，审阅差异后在源码副本中构建、测试并复查告警。原始工程不会自动应用候选；验证通过仍需审阅语义与测试覆盖。"),content);intro->setWordWrap(true);layout->addWidget(intro);
    auto* location=new QLabel(q(selected_.rule_id)+" · "+q(selected_.file)+":"+QString::number(selected_.line)+":"+QString::number(selected_.column),content);location->setWordWrap(true);layout->addWidget(location);
    choices_=new QComboBox(content);choices_->setObjectName("repairCandidates");layout->addWidget(choices_);
    risk_=new QLabel(content);risk_->setObjectName("repairRisk");risk_->setWordWrap(true);layout->addWidget(risk_);
    diff_=new QPlainTextEdit(content);diff_->setObjectName("repairDiff");diff_->setReadOnly(true);diff_->setLineWrapMode(QPlainTextEdit::NoWrap);diff_->setMinimumHeight(170);diff_->setStyleSheet("font-family: Consolas; font-size: 13px;");layout->addWidget(diff_,1);
    output_=new QLineEdit(content);output_->setObjectName("repairOutput");output_->setPlaceholderText(QStringLiteral("候选与验证记录目录，须在源码目录之外"));output_->setText(q(cg::utf8_path(options_.output_directory.empty()?database_.parent_path()/"repair-candidates":options_.output_directory)));layout->addWidget(output_);
    auto* coverage_row=new QHBoxLayout;coverage_=new QCheckBox(QStringLiteral("采集 gcov 覆盖率（仅 GNU 编译器）"),content);coverage_->setObjectName("repairCoverage");coverage_->setChecked(options_.collect_coverage);
    gcov_=new QLineEdit(q(options_.gcov),content);gcov_->setObjectName("repairGcov");gcov_->setPlaceholderText(QStringLiteral("gcov 命令或绝对路径"));coverage_row->addWidget(coverage_);coverage_row->addWidget(gcov_,1);layout->addLayout(coverage_row);
    auto* replay_row=new QHBoxLayout;per_test_=new QCheckBox(QStringLiteral("逐测试组重跑（含依赖 / fixture，CTest 3.29+）"),content);per_test_->setObjectName("repairPerTest");per_test_->setChecked(options_.collect_test_coverage);test_limit_=new QSpinBox(content);test_limit_->setObjectName("repairTestLimit");test_limit_->setRange(1,256);test_limit_->setValue(static_cast<int>(options_.coverage_test_limit));replay_row->addWidget(per_test_);replay_row->addWidget(new QLabel(QStringLiteral("声明测试数上限"),content));replay_row->addWidget(test_limit_);replay_row->addStretch();layout->addLayout(replay_row);connect(per_test_,&QCheckBox::toggled,this,[this]{controls();});
    auto* buttons=new QHBoxLayout;generate_=new QPushButton(QStringLiteral("重新生成"),content);generate_->setObjectName("repairGenerate");export_=new QPushButton(QStringLiteral("导出候选"),content);export_->setObjectName("repairExport");validate_=new QPushButton(QStringLiteral("在副本中验证"),content);validate_->setObjectName("repairValidate");cancel_=new QPushButton(QStringLiteral("取消任务"),content);cancel_->setObjectName("repairCancel");auto* close=new QPushButton(QStringLiteral("关闭"),content);close->setObjectName("repairClose");
    evaluate_=new QPushButton(QStringLiteral("验证全部并排序"),content);evaluate_->setObjectName("repairEvaluate");
    for(auto* button:{generate_,export_,validate_,evaluate_,cancel_,close})buttons->addWidget(button);layout->addLayout(buttons);
    auto* compare_row=new QHBoxLayout;compare_=new QPushButton(QStringLiteral("原版 / 补丁多轮覆盖对照"),content);compare_->setObjectName("repairCompare");rounds_=new QSpinBox(content);rounds_->setObjectName("repairRounds");rounds_->setRange(2,5);rounds_->setValue(2);compare_row->addWidget(compare_);compare_row->addWidget(new QLabel(QStringLiteral("每版轮数（独立 GNU 构建）"),content));compare_row->addWidget(rounds_);compare_row->addStretch();layout->addLayout(compare_row);connect(compare_,&QPushButton::clicked,this,[this]{compareCandidate();});
    status_=new QLabel(QStringLiteral("正在准备候选…"),content);status_->setObjectName("repairStatus");status_->setWordWrap(true);layout->addWidget(status_);
    results_=new QPlainTextEdit(content);results_->setObjectName("repairResults");results_->setReadOnly(true);results_->setMinimumHeight(130);results_->hide();layout->addWidget(results_);
    batch_table_=new QTableWidget(0,8,content);batch_table_->setObjectName("repairBatchTable");
    batch_table_->setHorizontalHeaderLabels({QStringLiteral("验证排序"),QStringLiteral("候选"),QStringLiteral("状态"),QStringLiteral("测试 总/失败/跳过"),QStringLiteral("修改字节"),QStringLiteral("行触达/插桩"),QStringLiteral("所选行"),QStringLiteral("正确性")});
    batch_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);batch_table_->setSelectionBehavior(QAbstractItemView::SelectRows);batch_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    batch_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);batch_table_->horizontalHeader()->setStretchLastSection(true);batch_table_->setMinimumHeight(110);batch_table_->setMaximumHeight(220);batch_table_->hide();layout->addWidget(batch_table_);
    connect(generate_,&QPushButton::clicked,this,[this]{generate();});connect(export_,&QPushButton::clicked,this,[this]{exportCandidate();});connect(validate_,&QPushButton::clicked,this,[this]{validateCandidate();});
    connect(choices_,&QComboBox::currentIndexChanged,this,[this]{preview();});connect(close,&QPushButton::clicked,this,[this]{requestClose();});
    connect(evaluate_,&QPushButton::clicked,this,[this]{evaluateCandidates();});
    connect(coverage_,&QCheckBox::toggled,this,[this]{controls();});
    connect(batch_table_,&QTableWidget::cellClicked,this,[this](int row,int){const auto id=batch_table_->item(row,0)->data(Qt::UserRole);for(int n=0;n<choices_->count();++n)if(choices_->itemData(n,Qt::UserRole+1)==id){choices_->setCurrentIndex(n);preview();break;}});
    connect(cancel_,&QPushButton::clicked,this,[this]{if(shared_&&shared_->control->request_cancel()){status_->setText(QStringLiteral("正在取消，等待当前解析或进程阶段结束并保留已产生的记录…"));cancel_->setEnabled(false);}});
    timer_.setInterval(20);connect(&timer_,&QTimer::timeout,this,[this]{poll();});controls();QTimer::singleShot(0,this,[this]{generate();});
}
RepairDialog::~RepairDialog(){timer_.stop();if(busy()){shared_->control->request_cancel();future_.wait();}}
void RepairDialog::controls(){const bool ready=choices_->currentIndex()>=0&&static_cast<std::size_t>(choices_->currentIndex())<candidates_.size();generate_->setEnabled(!busy());choices_->setEnabled(!busy());export_->setEnabled(!busy()&&ready);validate_->setEnabled(!busy()&&ready);evaluate_->setEnabled(!busy()&&!candidates_.empty());compare_->setEnabled(!busy()&&ready);rounds_->setEnabled(!busy());batch_table_->setEnabled(!busy());output_->setEnabled(!busy());coverage_->setEnabled(!busy());gcov_->setEnabled(!busy()&&coverage_->isChecked());per_test_->setEnabled(!busy()&&coverage_->isChecked());test_limit_->setEnabled(!busy()&&coverage_->isChecked()&&per_test_->isChecked());cancel_->setEnabled(busy()&&cancellable_);}
void RepairDialog::start(std::function<Outcome(const cg::ScanContext&)> work,bool cancellable,bool retain_pre_cancelled){
    if(busy())return;cancellable_=cancellable;shared_=std::make_shared<Shared>();const auto shared=shared_;
    future_=std::async(std::launch::async,[work=std::move(work),shared,retain_pre_cancelled]{Outcome out;cg::ScanContext context;context.control=shared->control;context.progress=[shared](const cg::ScanProgress& p){std::lock_guard lock(shared->mutex);shared->phase=p.phase;};
        try{if(!retain_pre_cancelled)context.check();out=work(context);}catch(const cg::ScanCancelled&){out.cancelled=true;}catch(const std::exception& e){out.error=e.what();}
        const auto final=shared->control->finish();if(final==cg::ScanControl::State::cancel_requested&&!out.validation&&!out.batch&&!out.comparison&&out.exported.empty()){out.cancelled=true;out.candidates.clear();}return out;});
    controls();timer_.start();
}
void RepairDialog::generate(){
    if(busy())return;candidates_.clear();validations_.clear();choices_->clear();diff_->clear();results_->clear();results_->hide();batch_table_->setRowCount(0);batch_table_->hide();risk_->clear();status_->setText(QStringLiteral("正在重新解析 AST 并核对源码与编译配置…"));const auto snapshot=snapshot_;const auto selected=selected_;
    start([snapshot,selected](const cg::ScanContext& c){Outcome out;out.candidates=cg::propose_repairs(*snapshot,selected,c);return out;});
}
void RepairDialog::preview(){const auto n=choices_->currentIndex();if(n<0||static_cast<std::size_t>(n)>=candidates_.size()){risk_->clear();diff_->clear();controls();return;}const auto& candidate=candidates_[static_cast<std::size_t>(n)];
    const auto impact=candidate.edit.recipe=="condition.assignment_result"?QStringLiteral("先保存返回值，再比较；会改变保存值、类型转换或求值顺序，需核对错误处理意图。"):
        candidate.edit.recipe=="condition.compare"?QStringLiteral("停止赋值并改变分支行为；需核对比较意图和测试覆盖。"):
        candidate.edit.recipe=="condition.intent"?QStringLiteral("保留赋值行为，只明确意图并消除该提示，不能据此认定行为缺陷被修复。"):
        QStringLiteral("可能截断文本，需检查返回值与错误处理，并确认 snprintf 可用。");
    risk_->setText(QStringLiteral("候选影响：")+impact);diff_->setPlainText(q(cg::repair_diff(candidate)));
    for(int row=0;row<batch_table_->rowCount();++row)if(batch_table_->item(row,0)->data(Qt::UserRole).toString()==q(candidate.id)){batch_table_->setCurrentCell(row,0);break;}
    if(auto found=validations_.find(candidate.id);found!=validations_.end())showValidation(found->second);else{results_->clear();results_->hide();}controls();}
void RepairDialog::exportCandidate(){
    const auto n=choices_->currentIndex();if(busy()||n<0)return;const auto candidate=candidates_.at(static_cast<std::size_t>(n));const auto output=cg::from_utf8(u(output_->text()));status_->setText(QStringLiteral("正在导出候选与差异…"));
    start([candidate,output](const cg::ScanContext&){Outcome out;out.exported=cg::export_repair_candidate(candidate,output);return out;},false);
}
void RepairDialog::validateCandidate(){
    const auto n=choices_->currentIndex();if(busy()||n<0)return;batch_table_->setRowCount(0);batch_table_->hide();const auto candidate=candidates_.at(static_cast<std::size_t>(n));auto options=options_;options.output_directory=cg::from_utf8(u(output_->text()));const auto database=database_;const auto root=cg::from_utf8(snapshot_->root);status_->setText(QStringLiteral("正在复制源码并验证所选候选…"));
    options.collect_coverage=coverage_->isChecked();options.collect_test_coverage=options.collect_coverage&&per_test_->isChecked();options.coverage_test_limit=static_cast<unsigned>(test_limit_->value());options.gcov=u(gcov_->text());
    start([candidate,options,root,database](const cg::ScanContext& context)mutable{Outcome out;options.control=context.control;options.progress=[context](const std::string& phase){context.report(phase);};out.validation=cg::validate_repair(root,database,candidate,options);return out;});
}
void RepairDialog::evaluateCandidates(){
    if(busy()||candidates_.empty())return;batch_table_->setRowCount(0);batch_table_->hide();results_->clear();results_->hide();const auto candidates=candidates_;auto options=options_;options.output_directory=cg::from_utf8(u(output_->text()));const auto database=database_;const auto root=cg::from_utf8(snapshot_->root);status_->setText(QStringLiteral("正在逐个验证全部候选并保存排序记录…"));
    options.collect_coverage=coverage_->isChecked();options.collect_test_coverage=options.collect_coverage&&per_test_->isChecked();options.coverage_test_limit=static_cast<unsigned>(test_limit_->value());options.gcov=u(gcov_->text());
    start([candidates,options,root,database](const cg::ScanContext& context)mutable{Outcome out;options.control=context.control;options.progress=[context](const std::string& phase){if(context.progress)context.progress({phase,{}});};out.batch=cg::evaluate_repairs(root,database,candidates,options);return out;},true,true);
}
void RepairDialog::compareCandidate(){
    const auto n=choices_->currentIndex();if(busy()||n<0)return;const auto candidate=candidates_.at(static_cast<std::size_t>(n));auto options=options_;options.output_directory=cg::from_utf8(u(output_->text()));options.gcov=u(gcov_->text());options.coverage_test_limit=static_cast<unsigned>(test_limit_->value());const auto rounds=static_cast<unsigned>(rounds_->value());const auto database=database_,root=cg::from_utf8(snapshot_->root);results_->clear();results_->hide();batch_table_->hide();status_->setText(QStringLiteral("正在独立构建原版与补丁并逐组采集覆盖记录…"));
    start([candidate,options,root,database,rounds](const cg::ScanContext& context)mutable{Outcome out;options.control=context.control;options.progress=[context](const std::string& phase){context.report(phase);};out.comparison=cg::compare_repair_coverage(root,database,{candidate},options,rounds);return out;},true,true);
}
void RepairDialog::showValidation(const cg::RepairValidation& r){
    auto stage=[&](const std::string& name){for(const auto& s:r.build.steps)if(s.name==name){auto value=state(s.result.status);if(name=="test")value+=QStringLiteral("（总数 %1，失败 %2，跳过 %3；-1 表示未知）").arg(s.tests_total).arg(s.tests_failed).arg(s.tests_skipped);return value;}return QStringLiteral("未运行");};
    auto count=[](std::int64_t n){return n<0?QStringLiteral("未知"):QString::number(n);};
    results_->setPlainText(QStringLiteral("同配置原始副本：")+(r.baseline_target_reproduced?QStringLiteral("已复现目标规则"):QStringLiteral("未确认复现"))+"\n"+QStringLiteral("编译：%1\n测试：%2\n静态复查：%3\n目标规则：%4\n新增或重新显现告警：%5\n原始 C/C++ 源码：%6\n修复正确性：未评估，需要审阅语义和测试覆盖\n记录：%7\n%8")
        .arg(stage("build"),stage("test"),q(r.analysis_status),r.target_not_detected?QStringLiteral("在本次已分析位置未再检出"):QStringLiteral("未验证消失"),count(r.new_or_unmasked_issues),r.build.source_unchanged?QStringLiteral("内容一致"):QStringLiteral("未确认"),q(r.workspace),q(r.error)));
    const auto& c=r.coverage;results_->appendPlainText(c.status=="not_measured"?QStringLiteral("覆盖率：未测量"):
        QStringLiteral("覆盖率采集：%1；工程行触达 %2/%3；分支记录触达 %4/%5（按对象统计）\n所选行：%6；未计入 %7 个工程文件。%8\n覆盖率记录：%9/summary.json")
            .arg(q(c.status)).arg(c.lines_hit).arg(c.lines_total).arg(c.branch_records_taken).arg(c.branch_records)
            .arg(c.target_line_status=="recorded"?QStringLiteral("已记录执行"):c.target_line_status=="no_recorded_execution"?QStringLiteral("未记录执行"):QStringLiteral("未确认插桩"))
            .arg(c.unmeasured_files.size()).arg(q(c.error),q(c.workspace)));if(r.test_coverage.status!="not_measured")results_->appendPlainText(repairTestCoverageText(r.test_coverage));results_->show();
}
void RepairDialog::showBatch(const cg::RepairBatch& batch){
    const auto order=cg::rank_repairs(batch.entries);batch_table_->setRowCount(static_cast<int>(order.size()));int passed=0,measured=0,requested=0;
    for(std::size_t rank=0;rank<order.size();++rank){const auto& e=batch.entries[order[rank]];const auto& v=e.validation;validations_[e.candidate_id]=v;if(v.gates_passed)++passed;
        QString tests=QStringLiteral("未运行");for(const auto& s:v.build.steps)if(s.name=="test")tests=QStringLiteral("%1 / %2 / %3").arg(s.tests_total).arg(s.tests_failed).arg(s.tests_skipped);
        if(v.coverage.status=="collected")++measured;if(v.coverage.status!="not_measured")++requested;
        const QStringList cells{QString::number(rank+1),q(e.recipe),q(v.status),tests,QString::number(e.changed_bytes),v.coverage.status=="collected"?QStringLiteral("%1/%2").arg(v.coverage.lines_hit).arg(v.coverage.lines_total):q(v.coverage.status),v.coverage.target_line_status=="recorded"?QStringLiteral("已记录执行"):v.coverage.target_line_status=="no_recorded_execution"?QStringLiteral("未记录执行"):QStringLiteral("未知"),QStringLiteral("未评估")};
        for(int col=0;col<cells.size();++col)batch_table_->setItem(static_cast<int>(rank),col,new QTableWidgetItem(cells[col]));batch_table_->item(static_cast<int>(rank),0)->setData(Qt::UserRole,q(e.candidate_id));
    }
    batch_table_->show();preview();
    status_->setText(QStringLiteral("批量状态：%1；已尝试 %2/%3，%4 个通过门槛。正确性未评估；%5。记录：%6/batch.json")
        .arg(q(batch.status)).arg(batch.attempted).arg(batch.entries.size()).arg(passed)
        .arg(!requested?QStringLiteral("测试覆盖率未测量"):measured==static_cast<int>(batch.entries.size())?(cg::comparable_repair_coverage(batch.entries)?QStringLiteral("覆盖率已采集，插桩行范围可比较"):QStringLiteral("覆盖率已采集，插桩行范围不同，不据此排序")):QStringLiteral("覆盖率采集不完整，不据此排序"))
        .arg(q(batch.workspace)));
}
void RepairDialog::poll(){
    if(!busy())return;
    if(future_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){
        if(shared_->control->state()!=cg::ScanControl::State::cancel_requested){std::string phase;{std::lock_guard lock(shared_->mutex);phase=shared_->phase;}
            const std::map<std::string,QString> names{{"repair_ast",QStringLiteral("重新解析 AST 并生成候选")},{"analyzing",QStringLiteral("解析翻译单元")},{"prepare",QStringLiteral("核对快照并复制源码")},{"repair_baseline_copy",QStringLiteral("保留未修改的对照副本")},{"configure",QStringLiteral("配置源码副本")},{"build",QStringLiteral("编译候选副本")},{"test",QStringLiteral("运行副本测试")},{"repair_analysis",QStringLiteral("在同一编译配置下复查原始与候选副本")}};
            if(auto found=names.find(phase);found!=names.end())status_->setText(found->second+QStringLiteral("…"));
            else if(phase.starts_with("candidate ")){const auto split=phase.find(": ");if(split!=std::string::npos){const auto label=names.find(phase.substr(split+2));status_->setText(QStringLiteral("候选 %1：%2…").arg(q(phase.substr(10,split-10)),label==names.end()?QStringLiteral("准备验证"):label->second));}}}return;}
    timer_.stop();auto outcome=future_.get();
    if(outcome.comparison){results_->setPlainText(repairComparisonText(*outcome.comparison));results_->show();status_->setText(outcome.comparison->status=="cancelled"?QStringLiteral("多轮对照已取消，已完成和未运行项均已留档"):outcome.comparison->ready_for_review?QStringLiteral("多轮覆盖对照完成；正确性仍需人工审阅"):QStringLiteral("多轮对照未通过全部门槛：")+q(outcome.comparison->status));}
    else if(outcome.batch)showBatch(*outcome.batch);
    else if(outcome.validation){const auto& r=*outcome.validation;validations_[r.candidate_id]=r;showValidation(r);
        status_->setText(r.gates_passed?QStringLiteral("本次验证门槛通过；候选仍需人工审阅"):r.status=="cancelled"?QStringLiteral("验证已取消，已产生的记录保留"):QStringLiteral("候选未通过全部验证门槛：")+q(r.status));
    }else if(!outcome.exported.empty())status_->setText(QStringLiteral("已导出：")+q(cg::utf8_path(outcome.exported)));
    else if(outcome.cancelled)status_->setText(QStringLiteral("候选任务已取消"));
    else if(!outcome.error.empty())status_->setText(QStringLiteral("候选任务失败：")+q(outcome.error));
    else {candidates_=std::move(outcome.candidates);for(const auto& c:candidates_){choices_->addItem(recipeName(c.edit.recipe),q(c.edit.recipe));choices_->setItemData(choices_->count()-1,q(c.id),Qt::UserRole+1);}status_->setText(candidates_.empty()?QStringLiteral("当前问题没有适用的确定性模板；不会猜测宏范围、指针容量或未知语义。"):QStringLiteral("已生成 %1 个候选；请选择并审阅差异与影响").arg(candidates_.size()));preview();}
    controls();if(close_pending_)QTimer::singleShot(0,this,[this]{QDialog::reject();});
}
void RepairDialog::requestClose(){if(!busy()){QDialog::reject();return;}close_pending_=true;shared_->control->request_cancel();status_->setText(QStringLiteral("等待任务安全结束后关闭，已产生的候选和日志保留…"));}
void RepairDialog::reject(){requestClose();}
void RepairDialog::closeEvent(QCloseEvent* event){if(busy()){requestClose();event->ignore();}else QDialog::closeEvent(event);}
