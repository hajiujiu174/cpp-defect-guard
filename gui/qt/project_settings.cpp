#include "project_settings.hpp"
#include "codeguard/application.hpp"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString q(const std::string& s) { return QString::fromUtf8(s); }
std::string s(const QString& q) { return q.toUtf8().toStdString(); }
QString lines(const std::vector<std::string>& values) {
    QStringList result; for(const auto& value:values)result<<q(value);return result.join('\n');
}
std::vector<std::string> lines(const QPlainTextEdit* edit) {
    std::vector<std::string> result;
    for(const auto& line:edit->toPlainText().split('\n'))if(!line.trimmed().isEmpty())result.push_back(s(line.trimmed()));
    return result;
}
}
ProjectSettings::ProjectSettings(const codeguard::fs::path& root,const codeguard::ProjectConfig& config,QWidget* parent)
    :QDialog(parent),root_(root),original_(config),selections_(config.command_choices),choices_path_(config.compile_commands) {
    setObjectName("projectSettings");setWindowTitle(QStringLiteral("工程设置 — ")+q(codeguard::utf8_path(root)));
    resize(840,650);setMinimumSize(700,560);
    auto* layout=new QVBoxLayout(this);auto* tabs=new QTabWidget(this);layout->addWidget(tabs);
    auto page=[&](const QString& title){auto* w=new QWidget(tabs);tabs->addTab(w,title);return new QFormLayout(w);};
    auto edit=[&](QFormLayout* form,const QString& label,const std::string& value,const char* name){
        auto* e=new QLineEdit(q(value),this);e->setObjectName(name);form->addRow(label,e);return e;
    };
    auto number=[&](QFormLayout* form,const QString& label,int value,int min,int max,const char* name){
        auto* e=new QSpinBox(this);e->setObjectName(name);e->setRange(min,max);e->setValue(value);
        if(min==0)e->setSpecialValueText(QStringLiteral("自动"));form->addRow(label,e);return e;
    };
    auto note=[&](QFormLayout* form,const QString& value){auto* l=new QLabel(value,this);l->setWordWrap(true);form->addRow(l);};
    auto* analysis=page(QStringLiteral("分析与规则"));
    analysis_=new QCheckBox(QStringLiteral("启用 Clang 分析"),this);analysis_->setChecked(config.analysis_enabled);
    analysis_->setEnabled(codeguard::clang_analysis_available());analysis->addRow(analysis_);
    discover_=new QCheckBox(QStringLiteral("路径为空时自动发现编译数据库"),this);discover_->setChecked(config.auto_discover);analysis->addRow(discover_);
    commands_=edit(analysis,QStringLiteral("编译数据库"),config.compile_commands,"configCommands");
    auto* buttons=new QHBoxLayout;auto* browse=new QPushButton(QStringLiteral("选择文件"),this);auto* find=new QPushButton(QStringLiteral("发现候选"),this);
    buttons->addWidget(browse);buttons->addWidget(find);analysis->addRow(buttons);
    connect(browse,&QPushButton::clicked,this,[&]{const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("选择编译数据库"),q(codeguard::utf8_path(root_)),"JSON (*.json)");if(!path.isEmpty())commands_->setText(path);});
    connect(find,&QPushButton::clicked,this,[&]{try{
        QStringList paths;for(const auto& path:codeguard::discover_compilation_databases(root_))paths<<q(path);
        if(paths.isEmpty()){QMessageBox::information(this,QStringLiteral("未发现编译数据库"),QStringLiteral("保存设置并导入文件清单后，点击“生成参数并分析”。非 CMake 工程请用原构建系统生成 compile_commands.json 后选择文件。"));return;}
        bool ok=true;auto path=paths.size()==1?paths.front():QInputDialog::getItem(this,QStringLiteral("选择构建配置"),QStringLiteral("仅分析选中的数据库"),paths,0,false,&ok);
        if(ok)commands_->setText(path);
    }catch(const std::exception& e){QMessageBox::warning(this,QStringLiteral("发现失败"),q(e.what()));}});
    threads_=number(analysis,QStringLiteral("分析线程"),config.threads,0,64,"configThreads");
    for(const auto& rule:codeguard::rule_catalog()){
        auto* check=new QCheckBox(q(rule.id+" · "+rule.title),this);check->setObjectName(q(rule.id));
        check->setChecked(std::find(config.disabled_rules.begin(),config.disabled_rules.end(),rule.id)==config.disabled_rules.end());
        analysis->addRow(check);rules_.emplace_back(rule.id,check);
    }
    note(analysis,QStringLiteral("规则开关作用于下一次扫描，旧快照保留当时的配置。缺少参数时只导入文件清单，不能据此判断没有缺陷。"));
    auto* build=page(QStringLiteral("构建与测试"));
    output_=edit(build,QStringLiteral("副本与日志目录"),codeguard::utf8_path(config.build.output_directory),"configOutput");
    c_=edit(build,QStringLiteral("C 编译器"),config.build.c_compiler,"configC");cxx_=edit(build,QStringLiteral("C++ 编译器"),config.build.cxx_compiler,"configCxx");
    target_=edit(build,QStringLiteral("目标（空为全部）"),config.build.target,"configTarget");
    type_=new QComboBox(this);type_->addItems({"Debug","Release","RelWithDebInfo","MinSizeRel"});type_->setCurrentText(q(config.build.build_type));build->addRow(QStringLiteral("构建类型"),type_);
    generator_=new QComboBox(this);generator_->setEditable(true);generator_->addItems({"Ninja","Ninja Multi-Config","MinGW Makefiles"});generator_->setCurrentText(q(config.build.generator));build->addRow(QStringLiteral("CMake 生成器"),generator_);
    jobs_=number(build,QStringLiteral("构建并行"),config.build.jobs,0,64,"configJobs");timeout_=number(build,QStringLiteral("每阶段超时（秒）"),static_cast<int>(config.build.timeout.count()/1000),1,86400,"configTimeout");
    cmake_=edit(build,"CMake",config.build.cmake,"configCmake");ctest_=edit(build,"CTest",config.build.ctest,"configCtest");git_=edit(build,"Git",config.build.git,"configGit");
    note(build,QStringLiteral("工具可填 PATH 中的命令名或绝对路径。编译器留空由 CMake 检测。生成参数仅执行配置；构建和测试需另行启动。"));
    auto* copy=page(QStringLiteral("参数与复制范围"));
    auto multiline=[&](const QString& label,const auto& values,const char* name){auto* e=new QPlainTextEdit(lines(values),this);e->setObjectName(name);e->setMaximumHeight(100);copy->addRow(label,e);return e;};
    definitions_=multiline(QStringLiteral("CMake 定义\n每行 KEY=VALUE"),config.build.cmake_definitions,"configDefinitions");
    includes_=multiline(QStringLiteral("补充复制目录\n每行项目相对路径"),config.build.copy_includes,"configIncludes");
    excludes_=multiline(QStringLiteral("排除目录\n同时排除扫描与复制"),config.build.copy_excludes,"configExcludes");
    note(copy,QStringLiteral("默认跳过 build、out 等目录及符号链接。补充目录可恢复被默认忽略的资源，排除优先。修改排除范围后先重新扫描。"));
    note(copy,QStringLiteral("外部依赖不自动复制：用 CMake 定义填写绝对依赖路径并保持可访问。支持配置阶段在构建目录生成的头文件；构建阶段生成的文件请用原构建系统准备并选择匹配数据库。配置阶段改写源码树会停止参数导入。CMake 脚本会执行，副本隔离不是操作系统沙箱。"));
    auto* command=page(QStringLiteral("编译命令选择"));
    note(command,QStringLiteral("同一文件有多个目标/宏配置时，请明确选择一条。未选择的文件保持歧义诊断。命令变化后旧选择报错，需重新选择。"));
    auto* reload=new QPushButton(QStringLiteral("载入当前数据库的命令"),this);reload->setObjectName("loadCommands");command->addRow(reload);
    choices_=new QTableWidget(0,2,this);choices_->setHorizontalHeaderLabels({QStringLiteral("源文件"),QStringLiteral("当前分析配置（悬停查看完整参数）")});
    choices_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);choices_->setMinimumHeight(300);command->addRow(choices_);
    connect(reload,&QPushButton::clicked,this,[&]{try{loadCommands();}catch(const std::exception& e){QMessageBox::warning(this,QStringLiteral("命令载入失败"),q(e.what()));}});
    auto* save=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);layout->addWidget(save);
    save->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));save->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(save,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(save,&QDialogButtonBox::accepted,this,[&]{try{configuration();accept();}catch(const std::exception& e){QMessageBox::warning(this,QStringLiteral("设置未保存"),q(e.what()));}});
}
void ProjectSettings::loadCommands() {
    auto config=configuration();auto options=codeguard::configured_scan_options(root_,config);
    if(options.compile_commands.empty())throw std::invalid_argument("请先选择或生成编译数据库");
    const auto commands=codeguard::inspect_compile_commands(options.compile_commands);
    selections_=config.command_choices;choices_path_=options.compile_commands;commands_->setText(q(choices_path_));choices_->setRowCount(0);
    std::map<std::string,std::vector<codeguard::CompileCommandInfo>> groups;for(const auto& c:commands)groups[c.file].push_back(c);
    for(const auto& [file,id]:selections_)groups.try_emplace(file); // allow clearing choices for removed files
    for(const auto& [file,variants]:groups){
        if(variants.size()<2&&!selections_.contains(file))continue;
        const int row=choices_->rowCount();choices_->insertRow(row);auto* item=new QTableWidgetItem(q(file));item->setFlags(item->flags()&~Qt::ItemIsEditable);item->setToolTip(q(file));choices_->setItem(row,0,item);
        auto* combo=new QComboBox(choices_);combo->addItem(QStringLiteral("未选择（保留歧义诊断）"),QString());
        if(selections_.contains(file)) {combo->addItem(QStringLiteral("保留已存选择（若失效将报错）"),q(selections_.at(file)));combo->setCurrentIndex(1);}
        for(const auto& c:variants){combo->addItem(q(c.fingerprint.substr(0,12)+" · "+c.display),q(c.fingerprint));combo->setItemData(combo->count()-1,q(c.display),Qt::ToolTipRole);}
        choices_->setCellWidget(row,1,combo);
    }
}
codeguard::ProjectConfig ProjectSettings::configuration() const {
    auto result=original_;result.analysis_enabled=analysis_->isChecked();result.auto_discover=discover_->isChecked();result.compile_commands=s(commands_->text().trimmed());
    if(!result.compile_commands.empty())result.compile_commands=codeguard::utf8_path(codeguard::fs::absolute(codeguard::from_utf8(result.compile_commands)));
    result.threads=threads_->value();auto& b=result.build;b.output_directory=codeguard::from_utf8(s(output_->text().trimmed()));
    if(!b.output_directory.empty())b.output_directory=codeguard::fs::absolute(b.output_directory);
    b.c_compiler=s(c_->text().trimmed());b.cxx_compiler=s(cxx_->text().trimmed());b.target=s(target_->text().trimmed());
    b.build_type=s(type_->currentText());b.generator=s(generator_->currentText().trimmed());b.jobs=jobs_->value();b.timeout=std::chrono::seconds(timeout_->value());
    b.cmake=s(cmake_->text().trimmed());b.ctest=s(ctest_->text().trimmed());b.git=s(git_->text().trimmed());
    b.cmake_definitions=lines(definitions_);b.copy_includes=lines(includes_);b.copy_excludes=lines(excludes_);
    result.disabled_rules.clear();for(const auto& [id,check]:rules_)if(!check->isChecked())result.disabled_rules.push_back(id);
    result.command_choices=result.compile_commands==choices_path_?selections_:std::map<std::string,std::string>{};
    if(result.compile_commands==choices_path_)for(int row=0;row<choices_->rowCount();++row){
        const auto file=s(choices_->item(row,0)->text());const auto id=s(static_cast<QComboBox*>(choices_->cellWidget(row,1))->currentData().toString());
        if(id.empty())result.command_choices.erase(file);else result.command_choices[file]=id;
    }
    codeguard::validate_config(result);
    if(!b.output_directory.empty()&&codeguard::project_path_inside(b.output_directory,root_))throw std::invalid_argument("副本与日志目录必须位于源码目录外");
    return result;
}
