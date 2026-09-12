#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include "source_editor.hpp"
#include "scan_task.hpp"
#include "build_task.hpp"
#include "project_settings.hpp"
#include <QSettings>
#include <QStandardPaths>
#include <QInputDialog>
#include <QSpinBox>
#include <QApplication>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QTabWidget>
#include <QLineEdit>
#include <QVBoxLayout>
#include <iostream>
#include <stdexcept>
#include <map>
#include <algorithm>
#include <QSplitter>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStatusBar>
#include <QTextBlock>
#include <QStyleFactory>
#include <QCloseEvent>
#include <functional>
#include <sstream>
#include <iomanip>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QProgressBar>
#include <QElapsedTimer>

namespace {
class TaskWindow final : public QMainWindow {
public:
    std::function<bool()> allowClose;
protected:
    void closeEvent(QCloseEvent* event) override {
        if (allowClose && !allowClose()) event->ignore();
        else QMainWindow::closeEvent(event);
    }
};
std::string bytes(const QString& text) { return text.toUtf8().toStdString(); }
QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<int>(value.size())); }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    const auto fontPath = QCoreApplication::applicationDirPath() + QStringLiteral("/resources/fonts/NotoSansCJKsc-Regular.otf");
    QString uiFontFamily = QStringLiteral("Microsoft YaHei UI");
    bool bundledFont = false;
    if (QFile::exists(fontPath)) {
        const auto id = QFontDatabase::addApplicationFont(fontPath);
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) { uiFontFamily = families.front(); bundledFont = true; }
    }
    if (qEnvironmentVariableIsSet("CODEGUARD_REQUIRE_BUNDLED_FONT") && !bundledFont) {
        std::cerr << "Packaged Chinese UI font is missing or invalid" << std::endl; return 1;
    }
    app.setFont(QFont(uiFontFamily, 10));
    app.setApplicationName("CodeGuard");
    app.setStyleSheet(QStringLiteral(R"CSS(
        QMainWindow, QWidget#workspace { background: #f3f6fa; color: #24344b; }
        QWidget { font-family: "%1"; font-size: 13px; }
        QLabel#brand { color: #1d4ed8; font-size: 25px; font-weight: 700; }
        QLabel#subtitle { color: #64748b; }
        QLabel#summary { background: white; border: 1px solid #e0e7ef; border-radius: 8px; padding: 12px; }
        QLabel#sourcePath { background: #eaf0f8; padding: 8px; color: #475569; }
        QPushButton { background: white; border: 1px solid #cbd5e1; border-radius: 5px; padding: 8px 13px; }
        QPushButton:hover { border-color: #3b82f6; background: #eff6ff; }
        QPushButton:disabled { color: #94a3b8; background: #f1f5f9; }
        QPushButton#primary { color: white; background: #2563eb; border-color: #2563eb; }
        QPushButton#primary:hover { background: #1d4ed8; }
        QPushButton#primary:disabled { color: #94a3b8; background: #f1f5f9; border-color: #cbd5e1; }
        QLineEdit { background: white; border: 1px solid #dbe3ee; border-radius: 4px; padding: 7px; }
        QTreeWidget, QTableWidget, QPlainTextEdit { background: white; border: 1px solid #e0e7ef;
            selection-background-color: #dbeafe; selection-color: #1e3a8a; }
        QTableWidget { alternate-background-color: #f8fafc; gridline-color: #eef2f7; }
        QHeaderView::section { background: #f1f5f9; color: #475569; border: none; padding: 7px; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: #eaf0f7; padding: 9px 11px; color: #64748b; }
        QTabBar::tab:selected { background: white; color: #2563eb; border-top: 2px solid #2563eb; }
        QSplitter::handle { background: #e3eaf3; }
        QStatusBar { background: #eaf0f7; color: #52647b; }
    )CSS").arg(uiFontFamily));
    TaskWindow window;
    window.setWindowTitle(QStringLiteral("CodeGuard — C/C++ 软件质量分析与工程管理"));
    window.resize(1480, 860);
    window.setMinimumSize(1080, 680);
    auto* container = new QWidget(&window);
    container->setObjectName("workspace");
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(16, 12, 16, 12); layout->setSpacing(10);
    auto* top = new QHBoxLayout;
    auto* brand = new QLabel("CodeGuard", container); brand->setObjectName("brand");
    auto* subtitle = new QLabel(QStringLiteral("C/C++ 工程质量工作台  /  源码只读"), container); subtitle->setObjectName("subtitle");
    top->addWidget(brand); top->addSpacing(12); top->addWidget(subtitle); top->addStretch();
    auto* open = new QPushButton(QStringLiteral("导入工程并扫描"), container);
    open->setObjectName("primary");
    auto* rescan = new QPushButton(QStringLiteral("重新扫描"), container); rescan->setEnabled(false);
    auto* reopen = new QPushButton(QStringLiteral("打开分析数据库"), container);
    top->addWidget(open); top->addWidget(rescan); top->addWidget(reopen);
    auto* choose_commands = new QPushButton(QStringLiteral("选择编译数据库"), container);
    choose_commands->setEnabled(codeguard::clang_analysis_available());
    auto* commands_path = new QLineEdit(container);
    commands_path->setPlaceholderText(QStringLiteral("compile_commands.json 路径；留空按工程设置自动发现"));
    commands_path->setEnabled(codeguard::clang_analysis_available());
    auto* thread_count = new QSpinBox(container); thread_count->setRange(0,64); thread_count->setSpecialValueText(QStringLiteral("自动"));
    thread_count->setToolTip(QStringLiteral("Clang 分析线程数；自动最多 8 个，显式可选 1–64")); thread_count->setPrefix(QStringLiteral("分析线程 "));
    auto* label = new QLabel(QStringLiteral("使用实际编译参数。调用图仅包含静态直接目标；未覆盖文件和间接调用会单独列出。"), container);
    label->setWordWrap(true);
    label->setObjectName("summary");
    auto* table = new QTableWidget(0, 4, container);
    table->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("语言"), QStringLiteral("物理行数"), QStringLiteral("字节数")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto* splitter = new QSplitter(Qt::Horizontal, container);
    splitter->setChildrenCollapsible(false); splitter->setHandleWidth(5);
    auto* explorer_panel = new QWidget(splitter);
    auto* explorer_layout = new QVBoxLayout(explorer_panel); explorer_layout->setContentsMargins(0, 0, 4, 0);
    auto* explorer_title = new QLabel(QStringLiteral("工程文件"), explorer_panel);
    auto* tree = new QTreeWidget(explorer_panel); tree->setHeaderHidden(true); tree->setMinimumWidth(150);
    tree->setObjectName("projectTree");
    explorer_layout->addWidget(explorer_title); explorer_layout->addWidget(tree);
    auto* editor_panel = new QWidget(splitter);
    auto* editor_layout = new QVBoxLayout(editor_panel); editor_layout->setContentsMargins(0, 0, 0, 0);
    auto* source_path = new QLabel(QStringLiteral("请选择左侧文件，或点击右侧结果定位"), editor_panel);
    source_path->setObjectName("sourcePath"); source_path->setWordWrap(true);
    auto* editor = new SourceEditor(editor_panel); editor->setObjectName("sourceEditor");
    editor->setStyleSheet("font-family: Consolas; font-size: 14px;");
    editor->setMinimumWidth(320);
    editor->setPlainText(QStringLiteral("// 欢迎使用 CodeGuard\n// 1. 选择编译数据库（可选）\n// 2. 导入 C/C++ 工程并保存分析数据库\n// 3. 点击文件或分析结果查看源码\n\n// 此窗口只读，不会修改工程文件。"));
    editor_layout->addWidget(source_path); editor_layout->addWidget(editor);
    auto* results_panel = new QWidget(splitter);
    auto* results_layout = new QVBoxLayout(results_panel); results_layout->setContentsMargins(4, 0, 0, 0);
    auto* filter = new QLineEdit(results_panel); filter->setPlaceholderText(QStringLiteral("筛选当前结果：名称、文件、类型…"));
    auto* tabs = new QTabWidget(results_panel); tabs->setMinimumWidth(380); tabs->setUsesScrollButtons(true);
    tabs->addTab(table, QStringLiteral("文件"));
    auto make_table = [&](const QString& title) {
        auto* value = new QTableWidget(tabs);
        value->setEditTriggers(QAbstractItemView::NoEditTriggers);
        value->setAlternatingRowColors(true); value->setSelectionBehavior(QAbstractItemView::SelectRows);
        value->setSelectionMode(QAbstractItemView::SingleSelection);
        value->verticalHeader()->setVisible(false);
        tabs->addTab(value, title);
        return value;
    };
    auto* symbols_table = make_table(QStringLiteral("符号"));
    auto* metrics_table = make_table(QStringLiteral("复杂度"));
    auto* edges_table = make_table(QStringLiteral("关系"));
    auto* cycles_table = make_table(QStringLiteral("循环"));
    auto* units_table = make_table(QStringLiteral("诊断"));
    auto* issues_table = make_table(QStringLiteral("问题")); issues_table->setObjectName("issuesTable");
    auto* build_panel = new QWidget(tabs); auto* build_layout = new QVBoxLayout(build_panel);
    auto* build_output = new QLineEdit(build_panel); build_output->setPlaceholderText(QStringLiteral("构建副本与日志目录，必须位于源码目录外"));
    auto* build_target = new QLineEdit(build_panel); build_target->setPlaceholderText(QStringLiteral("可选 CMake 目标，留空构建全部"));
    auto* build_compiler = new QLineEdit(build_panel); build_compiler->setPlaceholderText(QStringLiteral("可选 C++ 编译器路径，留空由 CMake 检测"));
    auto* build_jobs = new QSpinBox(build_panel); build_jobs->setRange(1,64); build_jobs->setValue(4); build_jobs->setPrefix(QStringLiteral("构建并行 "));
    auto* build_timeout = new QSpinBox(build_panel); build_timeout->setRange(1,86400); build_timeout->setValue(120); build_timeout->setSuffix(QStringLiteral(" 秒/阶段"));
    auto* build_start = new QPushButton(QStringLiteral("构建并测试副本"),build_panel);build_start->setObjectName("buildStart");build_start->setEnabled(false);
    auto* build_stop = new QPushButton(QStringLiteral("停止"),build_panel);build_stop->setEnabled(false);
    auto* build_history = new QPushButton(QStringLiteral("历史"),build_panel);build_history->setEnabled(false);
    auto* build_status = new QLabel(QStringLiteral("先保存扫描快照，再运行 CMake / CTest"),build_panel);build_status->setWordWrap(true);build_status->setTextFormat(Qt::PlainText);
    auto* build_table = new QTableWidget(build_panel);build_table->setObjectName("buildResults");build_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* build_log = new QPlainTextEdit(build_panel);build_log->setReadOnly(true);build_log->setObjectName("buildLog");
    auto* build_controls = new QHBoxLayout;build_controls->addWidget(build_jobs);build_controls->addWidget(build_timeout);
    auto* build_actions = new QHBoxLayout;build_actions->addWidget(build_start,1);build_actions->addWidget(build_stop);build_actions->addWidget(build_history);
    auto* build_settings_toggle=new QPushButton(QStringLiteral("设置"),build_panel);build_settings_toggle->setCheckable(true);build_actions->addWidget(build_settings_toggle);
    auto* build_settings=new QWidget(build_panel);auto* build_settings_layout=new QVBoxLayout(build_settings);build_settings_layout->setContentsMargins(0,0,0,0);
    build_settings_layout->addWidget(build_target);build_settings_layout->addWidget(build_compiler);build_settings_layout->addLayout(build_controls);build_settings->hide();
    QObject::connect(build_settings_toggle,&QPushButton::toggled,build_settings,&QWidget::setVisible);
    build_layout->addWidget(build_output);build_layout->addLayout(build_actions);build_layout->addWidget(build_settings);build_layout->addWidget(build_status);
    auto* build_views = new QTabWidget(build_panel);build_views->addTab(build_table,QStringLiteral("阶段结果"));build_views->addTab(build_log,QStringLiteral("日志"));
    build_layout->addWidget(build_views,1);tabs->addTab(build_panel,QStringLiteral("构建测试"));
    auto* git_log = new QPlainTextEdit(tabs);git_log->setReadOnly(true);tabs->addTab(git_log,QStringLiteral("Git"));
    auto* query_panel = new QWidget(tabs);
    auto* query_layout = new QVBoxLayout(query_panel);
    auto* query_help = new QLabel(QStringLiteral("只读查询：files / functions / symbols / edges / issues / builds"), query_panel);
    query_help->setToolTip(QStringLiteral("支持 SELECT、WHERE、AND/OR、ORDER BY、LIMIT；字符串使用单引号，数字字段使用整数。"));
    query_help->setWordWrap(true);
    auto* query_input = new QPlainTextEdit(query_panel);
    query_input->setObjectName("queryInput"); query_input->setFixedHeight(80);
    query_input->setPlainText("SELECT file, lines FROM files\nORDER BY lines DESC LIMIT 20;");
    auto* query_run = new QPushButton(QStringLiteral("执行只读查询"), query_panel);
    query_run->setObjectName("queryRun"); query_run->setEnabled(false);
    auto* query_status = new QLabel(QStringLiteral("请先导入工程或打开分析数据库"), query_panel);
    query_status->setTextFormat(Qt::PlainText); query_status->setWordWrap(true);
    auto* query_plan = new QPlainTextEdit(query_panel); query_plan->setReadOnly(true);
    query_plan->setFixedHeight(65); query_plan->setPlaceholderText(QStringLiteral("查询执行计划")); query_plan->hide();
    auto* query_explain = new QPushButton(QStringLiteral("查看执行计划"), query_panel); query_explain->setCheckable(true);
    QObject::connect(query_explain, &QPushButton::toggled, query_plan, &QWidget::setVisible);
    auto* query_actions = new QHBoxLayout; query_actions->addWidget(query_run, 1); query_actions->addWidget(query_explain);
    auto* query_table = new QTableWidget(query_panel); query_table->setObjectName("queryResults");
    query_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    query_table->setAlternatingRowColors(true);
    query_layout->addWidget(query_help); query_layout->addWidget(query_input); query_layout->addLayout(query_actions);
    query_layout->addWidget(query_status); query_layout->addWidget(query_plan); query_layout->addWidget(query_table, 1);
    tabs->addTab(query_panel, QStringLiteral("查询"));
    results_layout->addWidget(filter); results_layout->addWidget(tabs);
    auto* boundary = new QLabel(QStringLiteral("问题含五类 AST 规则 · 无告警不代表无缺陷 · 失败 TU 请查看诊断"), results_panel);
    boundary->setWordWrap(true); results_layout->addWidget(boundary);
    splitter->setSizes({190, 690, 560}); splitter->setStretchFactor(1, 1); splitter->setStretchFactor(2, 1);
    auto* configuration = new QHBoxLayout;
    configuration->addWidget(choose_commands); configuration->addWidget(commands_path);configuration->addWidget(thread_count);
    auto* project_settings=new QPushButton(QStringLiteral("工程设置"),container);project_settings->setObjectName("projectSettingsButton");project_settings->setEnabled(false);
    auto* generate=new QPushButton(QStringLiteral("生成参数并分析"),container);generate->setObjectName("generateCommands");generate->setEnabled(false);
    configuration->addWidget(project_settings);configuration->addWidget(generate);
    auto* task_row = new QHBoxLayout;
    auto* task_message = new QLabel(QStringLiteral("就绪 · 尚未启动扫描"), container); task_message->setWordWrap(true);
    task_message->setTextFormat(Qt::PlainText);
    auto* progress = new QProgressBar(container); progress->setRange(0, 100); progress->setValue(0); progress->setMaximumWidth(250);
    auto* cancel = new QPushButton(QStringLiteral("取消扫描"), container); cancel->setEnabled(false);
    task_row->addWidget(task_message, 1); task_row->addWidget(progress); task_row->addWidget(cancel);
    layout->addLayout(top); layout->addLayout(configuration); layout->addLayout(task_row); layout->addWidget(label); layout->addWidget(splitter, 1);
    window.setCentralWidget(container);
    window.statusBar()->showMessage(QStringLiteral("就绪 · 源码只读 · 尚未导入工程"));
    QString database;
    QString smoke_project;
    bool engineering_smoke = false;
    int engineering_stage = 0;
    QString initial_project;
    QString session_file, config_root, config_database, configuration_smoke;
    codeguard::ProjectConfig project_config;
    bool generating=false;
    codeguard::ScanResult current;
    ScanTask task;
    BuildTask build_task;
    std::vector<codeguard::BuildRun> build_runs;
    QTimer smoke_pulse; QElapsedTimer smoke_clock;
    qint64 smoke_last_tick = 0, smoke_max_gap = 0;
    int smoke_ticks = 0;
    QObject::connect(&smoke_pulse, &QTimer::timeout, [&]() {
        if (!task.busy()) return;
        const auto now = smoke_clock.elapsed();
        smoke_max_gap = std::max(smoke_max_gap, now - smoke_last_tick); smoke_last_tick = now; ++smoke_ticks;
    });
    bool close_pending = false;
    QString view_database;
    std::function<void(const codeguard::ScanResult&)> verify_smoke;
    std::function<void(const TaskOutcome&)> verify_background;
    QString opened_file;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size()) return 1;
        if (args[i] == "--database" && database.isEmpty()) database = args[i + 1];
        else if (args[i] == "--smoke-test" && smoke_project.isEmpty()) smoke_project = args[i + 1];
        else if (args[i] == "--engineering-smoke" && smoke_project.isEmpty()) {smoke_project=args[i+1];engineering_smoke=true;}
        else if (args[i] == "--compile-commands" && commands_path->text().isEmpty()) commands_path->setText(args[i + 1]);
        else if (args[i] == "--project" && initial_project.isEmpty()) initial_project = args[i + 1];
        else if (args[i] == "--session-file" && session_file.isEmpty()) session_file = args[i + 1];
        else if (args[i] == "--configuration-smoke" && configuration_smoke.isEmpty()) configuration_smoke = args[i + 1];
        else return 1;
    }
    const auto initial_commands=commands_path->text();
    if(session_file.isEmpty()&&smoke_project.isEmpty())session_file=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/session.ini";
    auto remember=[&]{
        if(session_file.isEmpty()||current.root.empty())return;
        QSettings session(session_file,QSettings::IniFormat);session.setValue("database",database);session.setValue("project",text(current.root));session.sync();
        if(session.status()!=QSettings::NoError)task_message->setText(QStringLiteral("工程已保存，但无法写入会话文件：")+session_file);
    };
    auto apply_config=[&](const codeguard::ProjectConfig& config){
        project_config=config;commands_path->setText(text(config.compile_commands));thread_count->setValue(config.threads);
        build_output->setText(text(codeguard::utf8_path(config.build.output_directory)));build_target->setText(text(config.build.target));
        build_compiler->setText(text(config.build.cxx_compiler));build_jobs->setRange(0,64);build_jobs->setSpecialValueText(QStringLiteral("自动"));build_jobs->setValue(config.build.jobs);
        build_timeout->setValue(static_cast<int>(config.build.timeout.count()/1000));
    };
    auto capture_config=[&]{
        auto config=project_config;const auto path=bytes(commands_path->text().trimmed());
        if(path!=config.compile_commands)config.command_choices.clear();config.compile_commands=path;
        config.threads=thread_count->value();config.build.output_directory=codeguard::from_utf8(bytes(build_output->text().trimmed()));
        config.build.target=bytes(build_target->text().trimmed());config.build.cxx_compiler=bytes(build_compiler->text().trimmed());
        config.build.jobs=build_jobs->value();config.build.timeout=std::chrono::seconds(build_timeout->value());return config;
    };
    auto load_config=[&](const QString& root){
        const auto canonical=codeguard::fs::canonical(codeguard::from_utf8(bytes(root)));
        const auto dbpath=codeguard::from_utf8(bytes(database));
        const auto stored=codeguard::load_project_config(canonical,dbpath);auto config=stored.value_or(codeguard::ProjectConfig{});
        if(!stored&&codeguard::fs::exists(dbpath)){
            codeguard::SqliteDatabase db(dbpath,true);const auto saved=db.latest(codeguard::utf8_path(canonical));
            if(!saved.analysis.configuration.empty())config=codeguard::decode_config(saved.analysis.configuration);
            else if(!saved.analysis.compile_commands.empty()){config.compile_commands=saved.analysis.compile_commands;config.auto_discover=false;}
        }
        if(config.build.output_directory.empty())config.build.output_directory=codeguard::from_utf8(bytes(QFileInfo(database).absolutePath()+"/codeguard-builds"));
        config_root=text(codeguard::utf8_path(canonical));config_database=database;apply_config(config);
    };
    auto choose_database = [&]() {
        if (database.isEmpty()) database = QFileDialog::getSaveFileName(&window,
            QStringLiteral("保存/追加分析数据库（必须位于源码目录之外）"), {}, "SQLite (*.sqlite3)", nullptr, QFileDialog::DontConfirmOverwrite);
        return !database.isEmpty();
    };
    QObject::connect(choose_commands, &QPushButton::clicked, [&]() {
        if (task.busy()||build_task.busy()) return;
        const auto path = QFileDialog::getOpenFileName(&window, QStringLiteral("选择编译数据库"), {}, "Compilation database (compile_commands.json)");
        if (!path.isEmpty()) {commands_path->setText(path);project_config.analysis_enabled=true;project_config.auto_discover=false;}
    });
    auto apply_filter = [&]() {
        auto* target = qobject_cast<QTableWidget*>(tabs->currentWidget());
        if (!target) return;
        for (int row = 0; row < target->rowCount(); ++row) {
            bool match = filter->text().isEmpty();
            for (int col = 0; col < target->columnCount() && !match; ++col)
                if (target->item(row, col)) match = target->item(row, col)->text().contains(filter->text(), Qt::CaseInsensitive);
            target->setRowHidden(row, !match);
        }
    };
    QObject::connect(filter, &QLineEdit::textChanged, [&]() { apply_filter(); });
    QObject::connect(tabs, &QTabWidget::currentChanged, [&]() { filter->setVisible(qobject_cast<QTableWidget*>(tabs->currentWidget()) != nullptr); apply_filter(); });
    auto navigate = [&](const QString& relative, int line) {
        try {
            const auto record = std::find_if(current.files.begin(), current.files.end(), [&](const auto& file) { return text(file.path) == relative; });
            if (record == current.files.end()) throw std::runtime_error("Location is outside the project inventory");
            const QDir root(QFileInfo(text(current.root)).canonicalFilePath());
            const auto path = QFileInfo(root.filePath(relative)).canonicalFilePath();
            const auto local = root.relativeFilePath(path);
            if (path.isEmpty() || local == ".." || local.startsWith("../") || QDir::isAbsolutePath(local))
                throw std::runtime_error("Source missing or path resolves outside the project");
            QFile file(path);
            constexpr qint64 limit = 2 * 1024 * 1024;
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read source file");
            const auto data = file.read(limit + 1);
            if (file.error() != QFileDevice::NoError || data.size() > limit) throw std::runtime_error("Source read failed or exceeds the 2 MiB viewer limit");
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto c : data) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ULL; }
            std::ostringstream fingerprint; fingerprint << "fnv1a64-v1:" << std::hex << std::setw(16) << std::setfill('0') << hash;
            const bool changed = fingerprint.str() != record->hash;
            editor->setPlainText(QString::fromUtf8(data)); editor->goToLine(line); opened_file = relative;
            source_path->setText(relative + QStringLiteral("   ·   只读 / UTF-8") + (changed ? QStringLiteral("   ·   内容已变化，请重新扫描") : ""));
            source_path->setToolTip(path);
            window.statusBar()->showMessage(QStringLiteral("%1  |  行 %2  |  %3").arg(relative).arg(editor->textCursor().blockNumber() + 1)
                .arg(changed ? QStringLiteral("源码与保存快照不同，旧位置仅供参考") : QStringLiteral("源码与扫描快照一致")));
            return true;
        } catch (const std::exception& exception) {
            window.statusBar()->showMessage(QStringLiteral("无法定位：") + text(exception.what())); return false;
        }
    };
    QObject::connect(tree, &QTreeWidget::itemClicked, [&](QTreeWidgetItem* item, int) {
        const auto path = item->data(0, Qt::UserRole).toString(); if (!path.isEmpty()) navigate(path, 1);
    });
    for (auto* target : {table, symbols_table, metrics_table, edges_table, units_table, issues_table})
        QObject::connect(target, &QTableWidget::cellClicked, [&, target](int row, int) {
            const auto* item = target->item(row, 0);
            if (item && !item->data(Qt::UserRole).toString().isEmpty())
                navigate(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toInt());
        });
    auto fill = [](QTableWidget* target, const QStringList& headers, const std::vector<QStringList>& rows) {
        target->clear(); target->setColumnCount(headers.size()); target->setHorizontalHeaderLabels(headers);
        target->setRowCount(static_cast<int>(rows.size()));
        for (std::size_t row = 0; row < rows.size(); ++row)
            for (int col = 0; col < rows[row].size(); ++col) {
                auto* item = new QTableWidgetItem(rows[row][col]); item->setToolTip(rows[row][col]);
                target->setItem(static_cast<int>(row), col, item);
            }
        target->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        target->horizontalHeader()->setMaximumSectionSize(480);
        target->horizontalHeader()->setStretchLastSection(true);
    };
    QObject::connect(query_run, &QPushButton::clicked, [&]() {
        query_table->clear(); query_table->setRowCount(0); query_table->setColumnCount(0); query_plan->clear();
        try {
            const auto answer = codeguard::execute_query(current, bytes(query_input->toPlainText()));
            QStringList columns; for (const auto& column : answer.columns) columns.push_back(text(column));
            std::vector<QStringList> rows;
            const auto shown = std::min<std::size_t>(answer.rows.size(), 2000);
            for (std::size_t i = 0; i < shown; ++i) {
                QStringList row; for (const auto& value : answer.rows[i]) row.push_back(text(codeguard::query_value_text(value)));
                rows.push_back(row);
            }
            fill(query_table, columns, rows); query_plan->setPlainText(text(answer.plan));
            query_status->setText(QStringLiteral("扫描批次 %1 · 分析状态 %2\n读取 %3 行，匹配 %4 行，返回 %5 行，显示 %6 行%7")
                .arg(answer.scan_id).arg(text(answer.analysis_status)).arg(answer.scanned_rows).arg(answer.matched_rows)
                .arg(answer.rows.size()).arg(shown).arg(shown < answer.rows.size() ? QStringLiteral("（界面上限 2000；请用 LIMIT 缩小结果）") : QString{}));
            if (!current.diagnostics.empty()) query_status->setText(query_status->text() + QStringLiteral("\n快照包含扫描诊断，请同时查看诊断页。"));
        } catch (const std::exception& error) {
            query_status->setText(QStringLiteral("查询失败：") + text(error.what()));
        }
    });
    QObject::connect(query_table, &QTableWidget::cellDoubleClicked, [&](int row, int) {
        QString file; int line = 1;
        for (int col = 0; col < query_table->columnCount(); ++col) {
            const auto name = query_table->horizontalHeaderItem(col)->text();
            if (name == "file") file = query_table->item(row, col)->text();
            if (name == "line") line = query_table->item(row, col)->text().toInt();
        }
        if (!file.isEmpty()) navigate(file, line);
    });
    auto display = [&](const codeguard::ScanResult& result) {
        QElapsedTimer render_clock; render_clock.start();
        current = result; rescan->setEnabled(true); tree->clear(); opened_file.clear(); filter->clear();
        build_runs.clear();build_table->clear();build_table->setRowCount(0);build_log->clear();git_log->clear();
        build_start->setEnabled(!build_task.busy());build_history->setEnabled(true);
        build_status->setText(QStringLiteral("可构建当前扫描快照；执行前核对源码，结果关联扫描批次 %1").arg(result.id));
        if(build_output->text().isEmpty()&&!database.isEmpty())build_output->setText(QFileInfo(database).absolutePath()+"/codeguard-builds");
        query_run->setEnabled(true); query_table->clear(); query_table->setRowCount(0); query_table->setColumnCount(0);
        query_plan->clear(); query_status->setText(QStringLiteral("快照已更新，请执行查询。双击含 file / line 的结果可定位源码。"));
        editor->clear(); source_path->setText(QStringLiteral("请选择工程文件"));
        std::map<QString, QTreeWidgetItem*> directories;
        auto* project_item = new QTreeWidgetItem(tree, {QFileInfo(text(result.root)).fileName()}); project_item->setExpanded(true);
        project_item->setIcon(0, window.style()->standardIcon(QStyle::SP_DirOpenIcon));
        directories[""] = project_item;
        auto location = [](QTableWidget* target, int row, const QString& file, int line) {
            target->item(row, 0)->setData(Qt::UserRole, file); target->item(row, 0)->setData(Qt::UserRole + 1, line);
        };
        table->setRowCount(static_cast<int>(result.files.size()));
        for (std::size_t i = 0; i < result.files.size(); ++i) {
            const auto& file = result.files[i];
            table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(text(file.path)));
            table->setItem(static_cast<int>(i), 1, new QTableWidgetItem(text(file.language)));
            table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(QString::number(file.lines)));
            table->setItem(static_cast<int>(i), 3, new QTableWidgetItem(QString::number(file.size)));
            location(table, static_cast<int>(i), text(file.path), 1);
            const auto parts = text(file.path).split('/'); QString prefix;
            for (int part = 0; part + 1 < parts.size(); ++part) {
                const auto next = prefix.isEmpty() ? parts[part] : prefix + "/" + parts[part];
                if (!directories.contains(next)) {
                    directories[next] = new QTreeWidgetItem(directories.at(prefix), {parts[part]});
                    directories[next]->setIcon(0, window.style()->standardIcon(QStyle::SP_DirIcon));
                }
                prefix = next;
            }
            auto* leaf = new QTreeWidgetItem(directories.at(prefix), {parts.back()});
            leaf->setIcon(0, window.style()->standardIcon(QStyle::SP_FileIcon));
            leaf->setData(0, Qt::UserRole, text(file.path)); leaf->setToolTip(0, text(file.path));
        }
        label->setText(QStringLiteral("%1\n文件: %2 | 物理行数: %3 | 新增: %4 | 变化: %5 | 未变化: %6 | 删除: %7 | 扫描批次: %8")
            .arg(text(result.root)).arg(result.files.size()).arg(result.total_lines()).arg(result.added)
            .arg(result.changed).arg(result.unchanged).arg(result.removed).arg(result.id));
        label->setText(label->text() + QStringLiteral("\n分析状态: %1 | 已覆盖: %2/%3 文件 | 函数度量: %4")
            .arg(text(result.analysis.status)).arg(result.analysis.covered_files.size()).arg(result.files.size()).arg(result.analysis.metrics.size()));
        label->setText(label->text()+QStringLiteral(" | 问题: %1 | 分析线程: %2 | 分析耗时: %3 ms")
            .arg(result.analysis.issues.size()).arg(result.analysis.workers).arg(result.analysis.elapsed_ms));
        if(!result.analysis.configuration.empty()){
            try{
                const auto settings=codeguard::decode_config(result.analysis.configuration);QStringList disabled;
                for(const auto& rule:settings.disabled_rules)disabled<<text(rule);
                label->setText(label->text()+QStringLiteral("\n本快照停用规则：%1 | 显式编译命令选择：%2")
                    .arg(disabled.isEmpty()?QStringLiteral("无"):disabled.join(", ")).arg(settings.command_choices.size()));
            }catch(const std::exception& e){label->setText(label->text()+QStringLiteral("\n无法读取历史配置：")+text(e.what()));}
        }
        std::map<std::string, codeguard::Symbol> symbols;
        std::vector<QStringList> symbol_rows, metric_rows, edge_rows, cycle_rows, unit_rows;
        for (const auto& symbol : result.analysis.symbols) {
            symbols[symbol.id] = symbol;
            if (!symbol.external) symbol_rows.push_back({text(symbol.kind), text(symbol.name), text(symbol.file),
                QString::number(symbol.line), symbol.definition ? QStringLiteral("定义") : QStringLiteral("声明"), text(symbol.id)});
        }
        auto metrics = result.analysis.metrics;
        std::sort(metrics.begin(), metrics.end(), [](const auto& a, const auto& b) { return a.complexity > b.complexity; });
        for (const auto& metric : metrics) {
            const auto& symbol = symbols.at(metric.symbol_id);
            metric_rows.push_back({text(symbol.name), QString::number(metric.complexity), QString::number(metric.lines),
                QString::number(metric.parameters), text(symbol.file) + ":" + QString::number(symbol.line)});
        }
        auto node_name = [&](const std::string& id) {
            const auto found = symbols.find(id);
            return found == symbols.end() ? text(id) : text(found->second.name) + " [" + text(found->second.file) + ":" + QString::number(found->second.line) + "]";
        };
        for (const auto& edge : result.analysis.edges)
            edge_rows.push_back({text(edge.kind), node_name(edge.source), node_name(edge.target), text(edge.file) + ":" + QString::number(edge.line)});
        for (const auto* kind : {"call", "include"}) {
            const auto graph = codeguard::project_graph(result, kind);
            for (const auto& component : graph.cycles) {
                QStringList members;
                for (const auto& node : component) members.push_back(node_name(node));
                cycle_rows.push_back({text(kind), members.join(" ; ")});
            }
        }
        for (const auto& unit : result.analysis.units)
            unit_rows.push_back({text(unit.file), text(unit.status), QString::number(unit.indirect_calls), text(unit.diagnostics)});
        if (result.analysis.status != "not_requested")
            for (const auto& file : result.files)
                if (std::find(result.analysis.covered_files.begin(), result.analysis.covered_files.end(), file.path) == result.analysis.covered_files.end())
                    unit_rows.push_back({text(file.path), "uncovered", "", QStringLiteral("未被成功翻译单元覆盖，不代表已完成分析")});
        fill(symbols_table, {QStringLiteral("类型"), QStringLiteral("名称"), QStringLiteral("文件"), QStringLiteral("行"), QStringLiteral("位置类型"), "USR"}, symbol_rows);
        fill(metrics_table, {QStringLiteral("函数"), QStringLiteral("CFG 圈复杂度"), QStringLiteral("函数行数"), QStringLiteral("参数"), QStringLiteral("位置")}, metric_rows);
        fill(edges_table, {QStringLiteral("图类型"), QStringLiteral("起点"), QStringLiteral("终点"), QStringLiteral("调用/include 位置")}, edge_rows);
        fill(cycles_table, {QStringLiteral("图类型"), QStringLiteral("循环强连通分量（成员列表，不是环路径）")}, cycle_rows);
        fill(units_table, {QStringLiteral("文件"), QStringLiteral("状态"), QStringLiteral("未解析调用数"), QStringLiteral("诊断")}, unit_rows);
        std::vector<QStringList> issue_rows;
        for(const auto& issue:result.analysis.issues)issue_rows.push_back({text(issue.severity),text(issue.rule_id),text(issue.file),QString::number(issue.line),text(issue.message),text(issue.evidence),text(issue.suggestion)});
        fill(issues_table,{QStringLiteral("级别"),QStringLiteral("规则"),QStringLiteral("文件"),QStringLiteral("行"),QStringLiteral("说明"),QStringLiteral("证据"),QStringLiteral("建议")},issue_rows);
        for(std::size_t i=0;i<result.analysis.issues.size();++i)location(issues_table,static_cast<int>(i),text(result.analysis.issues[i].file),result.analysis.issues[i].line);
        int row = 0;
        for (const auto& symbol : result.analysis.symbols) if (!symbol.external) location(symbols_table, row++, text(symbol.file), symbol.line);
        for (std::size_t i = 0; i < metrics.size(); ++i) {
            const auto& symbol = symbols.at(metrics[i].symbol_id); location(metrics_table, static_cast<int>(i), text(symbol.file), symbol.line);
        }
        for (std::size_t i = 0; i < result.analysis.edges.size(); ++i) {
            const auto& edge = result.analysis.edges[i]; location(edges_table, static_cast<int>(i), text(edge.file), edge.line);
        }
        for (int i = 0; i < units_table->rowCount(); ++i) location(units_table, i, units_table->item(i, 0)->text(), 1);
        tree->expandToDepth(1);
        if (!result.analysis.metrics.empty()) tabs->setCurrentWidget(metrics_table);
        else tabs->setCurrentWidget(table);
        if (!result.files.empty()) navigate(text(result.files.front().path), 1);
        apply_filter();
        if (!smoke_project.isEmpty()) std::cout << "GUI_DISPLAY_MS " << render_clock.elapsed() << " files=" << result.files.size()
            << " metrics=" << result.analysis.metrics.size() << std::endl;
    };
    auto set_running = [&](bool running) {
        open->setEnabled(!running); reopen->setEnabled(!running);
        rescan->setEnabled(!running && (!config_root.isEmpty()||!current.root.empty()));
        project_settings->setEnabled(!running&&!build_task.busy()&&!config_root.isEmpty());
        generate->setEnabled(!running&&!build_task.busy()&&current.id>0&&codeguard::clang_analysis_available());
        choose_commands->setEnabled(!running && codeguard::clang_analysis_available());
        commands_path->setEnabled(!running && codeguard::clang_analysis_available());
        cancel->setEnabled(running);
        thread_count->setEnabled(!running&&!build_task.busy());
        build_start->setEnabled(!running&&!build_task.busy()&&current.id>0);build_history->setEnabled(!running&&!build_task.busy()&&current.id>0);
    };
    task.updated = [&](TaskState state, const codeguard::ScanProgress& value) {
        if (!smoke_project.isEmpty() && state == TaskState::running && value.phase.empty()) {
            smoke_clock.start(); smoke_last_tick = smoke_max_gap = 0; smoke_ticks = 0; smoke_pulse.start(10);
        }
        set_running(true);
        const std::map<std::string, QString> phases{{"preparing", QStringLiteral("准备工程")}, {"scanning", QStringLiteral("扫描文件")},
            {"analysis_setup", QStringLiteral("载入编译参数")}, {"analyzing", QStringLiteral("Clang 解析")},
            {"verifying", QStringLiteral("核对源码一致性")}, {"saving", QStringLiteral("写入事务")}, {"before_commit", QStringLiteral("准备提交")}};
        if (state == TaskState::cancelling) {
            task_message->setText(QStringLiteral("正在取消 · 等待当前文件 / 翻译单元结束，旧结果保持不变")); cancel->setEnabled(false);
        } else if (state == TaskState::committing) {
            task_message->setText(QStringLiteral("正在提交事务 · 已越过取消边界，请等待完成")); cancel->setEnabled(false);
        } else {
            const auto found = phases.find(value.phase);
            auto message = found == phases.end() ? QStringLiteral("后台扫描已启动") : found->second;
            if (value.total) message += QStringLiteral(" %1/%2").arg(value.completed).arg(value.total);
            else if (value.phase == "scanning" || value.phase == "verifying")
                message += QStringLiteral(" · 已处理 %1").arg(value.completed);
            if (!value.file.empty()) message += " · " + text(value.file);
            task_message->setText(message);
        }
        if (value.total) { progress->setRange(0, static_cast<int>(std::min<std::size_t>(value.total, INT_MAX))); progress->setValue(static_cast<int>(std::min<std::size_t>(value.completed, INT_MAX))); }
        else progress->setRange(0, 0);
    };
    task.finished = [&](const TaskOutcome& outcome) {
        if (!smoke_project.isEmpty() && smoke_clock.isValid()) {
            smoke_max_gap = std::max(smoke_max_gap, smoke_clock.elapsed() - smoke_last_tick);
            std::cout << "GUI_BACKGROUND_TIMING elapsed_ms=" << smoke_clock.elapsed() << " ticks=" << smoke_ticks
                << " max_ui_gap_ms=" << smoke_max_gap << std::endl;
            smoke_pulse.stop();
        }
        set_running(false); progress->setRange(0, 100);
        if (outcome.result) {
            display(*outcome.result); view_database = database; progress->setValue(100);
            task_message->setText(outcome.state == TaskState::completed ? QStringLiteral("已完成 · 新快照已保存")
                : QStringLiteral("部分完成 · 成功结果与诊断已保存，请查看诊断页"));
            if(smoke_project.isEmpty()){
                if(current.analysis.status=="not_requested"&&project_config.analysis_enabled)
                    task_message->setText(QStringLiteral("文件清单已保存 · 缺少编译参数：在工程设置中选择数据库，或点击“生成参数并分析”（CMake 工程）"));
                remember();set_running(false);
            }
        } else {
            progress->setValue(0);
            task_message->setText(outcome.state == TaskState::cancelled ? QStringLiteral("已取消 · 未提交新快照，旧结果保持不变")
                : QStringLiteral("失败 · 未保存新快照，旧结果保持不变"));
            task_message->setToolTip(text(outcome.error));
            if (!current.root.empty()) {
                database = view_database;
                if(smoke_project.isEmpty()){try{load_config(text(current.root));}catch(const std::exception& e){task_message->setToolTip(text(e.what()));}}
                else commands_path->setText(text(current.analysis.compile_commands));
            }
        }
        if (!smoke_project.isEmpty()) {
            if (verify_background) verify_background(outcome);
            else if (outcome.state != TaskState::completed || !outcome.result) {
                std::cerr << "GUI async smoke failed: " << outcome.error << std::endl; app.exit(1);
            } else if (verify_smoke) verify_smoke(*outcome.result);
        } else if (outcome.state == TaskState::failed && !close_pending) {
            QMessageBox::warning(&window, QStringLiteral("后台扫描失败"), text(outcome.error));
        }
        if (close_pending) QTimer::singleShot(0, &window, &QWidget::close);
    };
    QObject::connect(cancel, &QPushButton::clicked, [&]() {
        if (!task.cancel() && task.busy()) task_message->setText(QStringLiteral("正在提交或结束任务，无法再取消；请等待结果"));
    });
    window.allowClose = [&]() {
        if (!task.busy()&&!build_task.busy()) return true;
        close_pending = true; task.cancel();build_task.cancel();
        task_message->setText(QStringLiteral("等待后台任务安全结束后关闭窗口…"));
        return false;
    };
    auto scan = [&](const QString& root) {
        if (task.busy()||build_task.busy()) return false;
        auto restore_selection=[&]{
            if(!current.root.empty()) {database=view_database;load_config(text(current.root));}
            set_running(false);
        };
        try {
        codeguard::ScanOptions options; options.compile_commands = bytes(commands_path->text());
        options.threads=static_cast<unsigned>(thread_count->value());
        if(smoke_project.isEmpty()){
            const auto canonical=text(codeguard::utf8_path(codeguard::fs::canonical(codeguard::from_utf8(bytes(root)))));
            if(config_root!=canonical||config_database!=database)load_config(root);
            auto config=capture_config();
            if(config.analysis_enabled&&config.compile_commands.empty()&&config.auto_discover&&codeguard::clang_analysis_available()){
                const auto candidates=codeguard::discover_compilation_databases(codeguard::from_utf8(bytes(root)));
                if(candidates.size()>1){
                    QStringList paths;for(const auto& path:candidates)paths<<text(path);bool ok=false;
                    const auto choice=QInputDialog::getItem(&window,QStringLiteral("发现多个编译数据库"),QStringLiteral("选择本次分析使用的构建配置"),paths,0,false,&ok);
                    if(!ok){restore_selection();return false;}config.compile_commands=bytes(choice);
                }
            }
            options=codeguard::configured_scan_options(codeguard::from_utf8(bytes(root)),config);
            config.compile_commands=options.compile_commands;apply_config(config);
            codeguard::save_project_config(codeguard::from_utf8(bytes(root)),codeguard::from_utf8(bytes(database)),config);
        }
        task_message->setToolTip({});
        return task.start(codeguard::from_utf8(bytes(root)), codeguard::from_utf8(bytes(database)), options);
        } catch (...) {restore_selection();throw;}
    };
    QObject::connect(project_settings,&QPushButton::clicked,[&]{
        if(task.busy()||build_task.busy()||config_root.isEmpty())return;
        try{
            ProjectSettings dialog(codeguard::from_utf8(bytes(config_root)),capture_config(),&window);
            if(dialog.exec()!=QDialog::Accepted)return;
            auto config=dialog.configuration();codeguard::save_project_config(codeguard::from_utf8(bytes(config_root)),codeguard::from_utf8(bytes(database)),config);apply_config(config);
            task_message->setText(QStringLiteral("工程设置已保存 · 重新扫描后生效；生成参数将执行 CMake 配置"));remember();
        }catch(const std::exception& e){QMessageBox::warning(&window,QStringLiteral("配置未保存"),text(e.what()));}
    });
    QObject::connect(open, &QPushButton::clicked, [&]() {
        if (task.busy()||build_task.busy()) return;
        auto root = QFileDialog::getExistingDirectory(&window, QStringLiteral("选择 C/C++ 工程目录"));
        if (root.isEmpty() || !choose_database()) return;
        try { scan(root); }
        catch (const std::exception& exception) { QMessageBox::warning(&window, QStringLiteral("扫描未完成"), text(exception.what())); }
    });
    QObject::connect(rescan, &QPushButton::clicked, [&]() {
        if (task.busy()||build_task.busy()) return;
        if ((current.root.empty()&&config_root.isEmpty()) || !choose_database()) return;
        const auto root = config_root.isEmpty()?text(current.root):config_root;
        try { scan(root); }
        catch (const std::exception& exception) { QMessageBox::warning(&window, QStringLiteral("扫描未完成"), text(exception.what())); }
    });
    QObject::connect(reopen, &QPushButton::clicked, [&]() {
        if (task.busy()||build_task.busy()) return;
        const auto selected = QFileDialog::getOpenFileName(&window, QStringLiteral("打开已有分析数据库，载入其中最近工程"), database, "SQLite (*.sqlite3)");
        if (selected.isEmpty()) return;
        try {
            codeguard::SqliteDatabase db(codeguard::from_utf8(bytes(selected)), true);
            const auto projects = db.projects();
            if (projects.empty()) throw std::runtime_error("No saved projects");
            display(db.latest(projects.front().root));
            database = selected;
            view_database = selected;
            if(build_output->text().isEmpty())build_output->setText(QFileInfo(selected).absolutePath()+"/codeguard-builds");
            commands_path->setText(text(current.analysis.compile_commands));
            load_config(text(current.root));remember();set_running(false);
            task_message->setText(QStringLiteral("已载入历史快照 · 未启动新扫描")); task_message->setToolTip({});
            progress->setRange(0, 100); progress->setValue(100);
        } catch (const std::exception& exception) { QMessageBox::warning(&window, QStringLiteral("打开失败"), text(exception.what())); }
    });
    auto show_builds = [&](std::vector<codeguard::BuildRun> runs) {
        build_runs=std::move(runs);std::vector<QStringList> rows;
        for(const auto& run:build_runs)for(const auto& step:run.steps)rows.push_back({QString::number(run.id),QString::number(run.scan_id),text(step.name),text(step.result.status),
            QString::number(step.result.duration_ms),step.tests_total<0?QStringLiteral("—"):QString::number(step.tests_total),step.tests_failed<0?QStringLiteral("—"):QString::number(step.tests_failed),text(run.started_at)});
        fill(build_table,{QStringLiteral("运行"),QStringLiteral("扫描"),QStringLiteral("阶段"),QStringLiteral("状态"),QStringLiteral("毫秒"),QStringLiteral("测试数"),QStringLiteral("失败数"),QStringLiteral("时间")},rows);
        int row=0;for(std::size_t i=0;i<build_runs.size();++i)for(std::size_t j=0;j<build_runs[i].steps.size();++j){auto* item=build_table->item(row++,0);item->setData(Qt::UserRole,static_cast<int>(i));item->setData(Qt::UserRole+1,static_cast<int>(j));}
        if(!build_runs.empty()){
            const auto& run=build_runs.front();build_status->setText(QStringLiteral("运行 %1 · %2 · 关联扫描 %3\n源码与快照一致：%4（悬停查看日志目录）").arg(run.id).arg(text(run.status)).arg(run.scan_id).arg(run.source_unchanged?QStringLiteral("是"):QStringLiteral("否")));
            build_status->setToolTip(text(run.workspace));
            git_log->setPlainText(text(run.git_log));
        }
    };
    auto refresh_builds = [&]() {
        codeguard::SqliteDatabase db(codeguard::from_utf8(bytes(database)),true);
        current.build_runs.clear();auto history=db.builds(current.root);
        for(const auto& run:history)if(run.scan_id==current.id)current.build_runs.push_back(run);
        show_builds(std::move(history));
    };
    QObject::connect(build_table,&QTableWidget::cellClicked,[&](int row,int){
        try{
            const auto* item=build_table->item(row,0);if(!item)return;
            auto& run=build_runs.at(item->data(Qt::UserRole).toInt());const auto step_index=item->data(Qt::UserRole+1).toInt();
            codeguard::SqliteDatabase db(codeguard::from_utf8(bytes(database)),true);auto records=db.builds(current.root,true,run.id);
            if(records.empty())throw std::runtime_error("Build record missing");run=std::move(records.front());
            const auto& step=run.steps.at(step_index);git_log->setPlainText(text(run.git_log));
            build_table->setCurrentCell(row,0);build_table->scrollToItem(build_table->item(row,0));
            build_log->setPlainText(text(step.name+" | "+step.result.status+" | exit="+std::to_string(step.result.exit_code)+"\nstdout:\n"+step.result.stdout_text+"\nstderr:\n"+step.result.stderr_text+
                (step.result.output_truncated?"\n[output truncated]\n":"")+"\ncommand:\n"+step.command));
        }catch(const std::exception&e){build_log->setPlainText(text(e.what()));}
    });
    build_task.updated=[&](const std::string& phase){
        project_settings->setEnabled(false);generate->setEnabled(false);
        open->setEnabled(false);rescan->setEnabled(false);reopen->setEnabled(false);choose_commands->setEnabled(false);commands_path->setEnabled(false);thread_count->setEnabled(false);
        build_start->setEnabled(false);build_history->setEnabled(false);build_output->setEnabled(false);build_target->setEnabled(false);build_compiler->setEnabled(false);build_jobs->setEnabled(false);build_timeout->setEnabled(false);
        build_status->setText(QStringLiteral("后台构建测试：%1 · 在源码副本中执行").arg(text(phase)));
    };
    build_task.finished=[&](const BuildOutcome& outcome){
        set_running(false);build_stop->setEnabled(false);build_output->setEnabled(true);build_target->setEnabled(true);build_compiler->setEnabled(true);build_jobs->setEnabled(true);build_timeout->setEnabled(true);
        if(outcome.result){
            try{refresh_builds();if(build_table->rowCount())build_table->cellClicked(0,0);}catch(const std::exception&e){build_log->setPlainText(text(e.what()));}
        }else build_status->setText(QStringLiteral("构建管理失败：")+text(outcome.error));
        const bool follow_analysis=generating;generating=false;
        if(follow_analysis&&!close_pending){
            if(outcome.result&&outcome.result->status=="configured"){
                try{
                    auto config=capture_config();config.compile_commands=outcome.result->compile_commands;config.analysis_enabled=true;config.auto_discover=false;config.command_choices.clear();
                    codeguard::save_project_config(codeguard::from_utf8(current.root),codeguard::from_utf8(bytes(database)),config);apply_config(config);scan(text(current.root));
                }catch(const std::exception& e){task_message->setText(QStringLiteral("参数已生成，分析未启动：")+text(e.what()));}
            }else task_message->setText(QStringLiteral("参数生成未完成 · 查看构建测试页的阶段日志，修正工程设置后重试"));
        }
        if(close_pending)QTimer::singleShot(0,&window,&QWidget::close);
    };
    auto start_build = [&]() {
        if(task.busy()||build_task.busy()||current.id<=0)return false;
        codeguard::BuildOptions options=project_config.build;options.output_directory=codeguard::from_utf8(bytes(build_output->text()));options.target=bytes(build_target->text());
        options.cxx_compiler=bytes(build_compiler->text());options.jobs=static_cast<unsigned>(build_jobs->value());options.timeout=std::chrono::seconds(build_timeout->value());
        if(smoke_project.isEmpty()){auto config=capture_config();codeguard::save_project_config(codeguard::from_utf8(current.root),codeguard::from_utf8(bytes(database)),config);project_config=config;}
        build_log->clear();git_log->clear();build_stop->setEnabled(true);tabs->setCurrentWidget(build_panel);
        return build_task.start(codeguard::from_utf8(current.root),codeguard::from_utf8(bytes(database)),options);
    };
    QObject::connect(build_start,&QPushButton::clicked,[&]{try{start_build();}catch(const std::exception&e){build_status->setText(text(e.what()));}});
    QObject::connect(generate,&QPushButton::clicked,[&]{
        if(task.busy()||build_task.busy()||current.id<=0)return;
        try{
            auto config=capture_config();codeguard::save_project_config(codeguard::from_utf8(current.root),codeguard::from_utf8(bytes(database)),config);project_config=config;
            auto options=config.build;options.configure_only=true;generating=true;build_stop->setEnabled(true);tabs->setCurrentWidget(build_panel);
            if(!build_task.start(codeguard::from_utf8(current.root),codeguard::from_utf8(bytes(database)),options))generating=false;
        }catch(const std::exception& e){generating=false;task_message->setText(QStringLiteral("无法生成参数：")+text(e.what()));}
    });
    QObject::connect(build_stop,&QPushButton::clicked,[&]{if(build_task.cancel()){build_stop->setEnabled(false);build_status->setText(QStringLiteral("正在停止进程并保存日志…"));}});
    QObject::connect(build_history,&QPushButton::clicked,[&]{if(task.busy()||build_task.busy())return;try{refresh_builds();if(build_table->rowCount())build_table->cellClicked(0,0);}catch(const std::exception&e){build_status->setText(text(e.what()));}});
    window.show();
    if (qEnvironmentVariableIsSet("CODEGUARD_REQUIRE_BUNDLED_FONT")) {
        const QFontMetrics metrics(open->font());
        for (const auto ch : QStringLiteral("导入工程扫描选择编译数据库查询线程构建测试源码只读")) {
            if (!metrics.inFont(ch)) { std::cerr << "Chinese UI glyph missing" << std::endl; return 1; }
        }
        std::cout << "GUI_FONT_OK family=" << bytes(uiFontFamily) << std::endl;
    }
    // Deterministic widget + persistence smoke check; not a substitute for visual QA.
    if (!smoke_project.isEmpty()) {
        if (database.isEmpty()) return 1;
        verify_smoke = [&](const codeguard::ScanResult& result) {
            try {
                codeguard::SqliteDatabase db(codeguard::from_utf8(bytes(database)), true);
                const auto saved = db.latest(result.root);
                if (!saved.id || saved.files.size() != result.files.size() || table->rowCount() != static_cast<int>(saved.files.size()))
                    throw std::runtime_error("widget/database mismatch");
                if (!commands_path->text().isEmpty()) {
                    if (saved.analysis.status != "complete" || saved.analysis.metrics.empty() || metrics_table->rowCount() != static_cast<int>(saved.analysis.metrics.size()))
                        throw std::runtime_error("analysis widget/database mismatch");
                    tabs->setCurrentWidget(metrics_table);
                }
                // Exercise the same signals/slots as tree and result clicks, without screen coordinates.
                display(saved);
                if (!editor->isReadOnly() || tree->topLevelItemCount() != 1)
                    throw std::runtime_error("read-only editor / project tree missing");
                if (!saved.files.empty()) {
                    table->cellClicked(0, 0);
                    if (opened_file != text(saved.files[0].path) || editor->textCursor().blockNumber() != 0)
                        throw std::runtime_error("file row navigation failed");
                    auto* leaf = tree->topLevelItem(0);
                    while (leaf->childCount()) leaf = leaf->child(0);
                    tree->itemClicked(leaf, 0);
                    if (opened_file != leaf->data(0, Qt::UserRole).toString()) throw std::runtime_error("tree navigation failed");
                }
                if (!saved.analysis.metrics.empty()) {
                    tabs->setCurrentWidget(metrics_table);
                    filter->setText("___no_matching_result___");
                    for (int row = 0; row < metrics_table->rowCount(); ++row)
                        if (!metrics_table->isRowHidden(row)) throw std::runtime_error("result filter failed");
                    filter->clear(); metrics_table->cellClicked(0, 0);
                    if (opened_file != metrics_table->item(0, 0)->data(Qt::UserRole).toString() ||
                        editor->textCursor().blockNumber() + 1 != metrics_table->item(0, 0)->data(Qt::UserRole + 1).toInt())
                        throw std::runtime_error("metric source line navigation failed");
                }
                if (navigate("../README.md", 1)) throw std::runtime_error("out-of-project source was opened");
                if (!saved.analysis.metrics.empty()) metrics_table->cellClicked(0, 0);
                app.processEvents();
                const auto screenshot = codeguard::from_utf8(bytes(database)).parent_path() / "gui-smoke.png";
                if (!window.grab().save(text(codeguard::utf8_path(screenshot))))
                    throw std::runtime_error("cannot save GUI smoke screenshot");
                window.resize(1080, 680); app.processEvents();
                const auto compact = screenshot.parent_path() / "gui-smoke-compact.png";
                if (!window.grab().save(text(codeguard::utf8_path(compact)))) throw std::runtime_error("cannot save compact screenshot");
                tabs->setCurrentWidget(query_panel);
                query_input->setPlainText("SELECT file, lines FROM files ORDER BY lines DESC LIMIT 1;"); query_run->click();
                if (query_table->rowCount() != (saved.files.empty() ? 0 : 1) || query_plan->toPlainText().isEmpty())
                    throw std::runtime_error("query widget results mismatch");
                if (!saved.analysis.metrics.empty()) {
                    query_input->setPlainText("SELECT name, file, line, complexity FROM functions WHERE complexity >= 0 ORDER BY complexity DESC LIMIT 3;"); query_run->click();
                    if (query_table->rowCount() == 0) throw std::runtime_error("function query returned no rows");
                    query_table->cellDoubleClicked(0, 0);
                    if (opened_file != query_table->item(0, 1)->text() || editor->textCursor().blockNumber() + 1 != query_table->item(0, 2)->text().toInt())
                        throw std::runtime_error("query source navigation failed");
                }
                app.processEvents();
                if (!window.grab().save(text(codeguard::utf8_path(screenshot.parent_path() / "gui-query-compact.png")))) throw std::runtime_error("query screenshot failed");
                window.resize(1480, 860); app.processEvents();
                if (!window.grab().save(text(codeguard::utf8_path(screenshot.parent_path() / "gui-query.png")))) throw std::runtime_error("query screenshot failed");
                query_input->setPlainText("SELECT missing FROM files;"); query_run->click();
                if (query_table->rowCount() != 0 || !query_status->text().startsWith(QStringLiteral("查询失败：")))
                    throw std::runtime_error("invalid query did not clear old results");
                tabs->setCurrentWidget(metrics_table);
                if(engineering_smoke){
                    if(saved.analysis.issues.size()!=5||issues_table->rowCount()!=5)throw std::runtime_error("five rules missing from GUI");
                    tabs->setCurrentWidget(issues_table);issues_table->cellClicked(0,0);app.processEvents();
                    window.grab().save(text(codeguard::utf8_path(screenshot.parent_path()/"gui-issues.png")));
                    const auto original_finish=build_task.finished;const auto original_update=build_task.updated;
                    build_task.updated=[&,original_update](const std::string& phase){
                        original_update(phase);
                        if(engineering_stage==1&&phase=="configure"){
                            engineering_stage=2;build_stop->click();
                            if(window.close())throw std::runtime_error("window closed during build cancellation");
                        }
                    };
                    build_task.finished=[&,original_finish](const BuildOutcome& outcome){
                        original_finish(outcome);
                        try{
                            if(!outcome.result)throw std::runtime_error("GUI build failed: "+outcome.error);
                            const auto& run=*outcome.result;
                            if(engineering_stage==0){
                                if(run.status!="passed"||run.steps.back().tests_total!=1||build_table->rowCount()<4||!build_start->isEnabled())throw std::runtime_error("GUI build/test result mismatch");
                                if(codeguard::execute_query(current,"SELECT stage FROM builds WHERE status = 'passed'").rows.size()<4)throw std::runtime_error("GUI build query stale");
                                tabs->setCurrentWidget(build_panel);build_table->cellClicked(3,0);app.processEvents();
                                const auto dir=codeguard::from_utf8(bytes(database)).parent_path();
                                window.grab().save(text(codeguard::utf8_path(dir/"gui-build.png")));
                                window.resize(1080,680);app.processEvents();window.grab().save(text(codeguard::utf8_path(dir/"gui-build-compact.png")));
                                engineering_stage=1;
                                QTimer::singleShot(0,[&]{build_start->click();if(!build_task.busy()||scan(smoke_project)||build_start->isEnabled())app.exit(1);});
                            }else{
                                if(run.status!="cancelled"||!run.id||!run.source_unchanged||build_task.busy())throw std::runtime_error("GUI cancelled build did not persist logs");
                                std::cout<<"GUI_LEVEL3_OK rules build test history query cancel close-deferred"<<std::endl;
                            }
                        }catch(const std::exception&e){std::cerr<<e.what()<<std::endl;app.exit(1);}
                    };
                    build_start->click();if(!build_task.busy()||scan(smoke_project))throw std::runtime_error("build/scan mutual exclusion failed");
                    return;
                }
                // Exercise the real window while a second scan is inside its
                // write transaction. The bounded gate makes cancellation deterministic.
                const auto source_before = editor->toPlainText();
                const auto file_before = opened_file;
                verify_background = [&, saved, source_before, file_before](const TaskOutcome& outcome) {
                    try {
                        codeguard::SqliteDatabase db(codeguard::from_utf8(bytes(database)), true);
                        if (outcome.state != TaskState::cancelled || outcome.result || task.busy() || current.id != saved.id ||
                            db.latest(saved.root).id != saved.id || table->rowCount() != static_cast<int>(saved.files.size()) ||
                            editor->toPlainText() != source_before || opened_file != file_before || filter->text() != "___keep_filter___" ||
                            !open->isEnabled() || !rescan->isEnabled() || !reopen->isEnabled() || cancel->isEnabled() ||
                            !task_message->text().startsWith(QStringLiteral("已取消")) || progress->value() != 0)
                            throw std::runtime_error("cancelled window/database state mismatch");
                        const auto screenshot = codeguard::from_utf8(bytes(database)).parent_path() / "gui-smoke-cancelled.png";
                        if (!window.grab().save(text(codeguard::utf8_path(screenshot)))) throw std::runtime_error("cancel screenshot failed");
                        std::cout << "GUI_SMOKE_OK files=" << saved.files.size() << " responsive duplicate rollback preserved close-deferred" << std::endl;
                        // The production close-pending handler now closes the window.
                    } catch (const std::exception& exception) { std::cerr << exception.what() << std::endl; app.exit(1); }
                };
                auto release = std::make_shared<std::promise<void>>();
                const auto released = release->get_future().share();
                auto entered = std::make_shared<std::atomic<bool>>(false);
                codeguard::ScanOptions options; options.compile_commands = bytes(commands_path->text());
                options.context.progress = [entered, released](const codeguard::ScanProgress& value) {
                    if (value.phase == "before_commit") {
                        *entered = true;
                        if (released.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                            throw std::runtime_error("window smoke gate timeout");
                    }
                };
                auto* heartbeat = new QTimer(&window); heartbeat->setInterval(2);
                QObject::connect(heartbeat, &QTimer::timeout, [&, heartbeat, entered, release, ticks = 0]() mutable {
                    if (!entered->load()) return;
                    try {
                        if (!task.busy() || scan(smoke_project) || open->isEnabled() || rescan->isEnabled() || reopen->isEnabled())
                            throw std::runtime_error("window duplicate scan guard failed");
                        filter->setText("___keep_filter___"); tabs->setCurrentWidget(table);
                        if (!filter->isEnabled() || !tree->isEnabled() || !editor->isEnabled() ||
                            (table->rowCount() && !table->isRowHidden(0))) throw std::runtime_error("window did not process filter during scan");
                        if (++ticks < 2 || !task_message->text().startsWith(QStringLiteral("准备提交"))) return;
                        const auto screenshot = codeguard::from_utf8(bytes(database)).parent_path() / "gui-smoke-running.png";
                        if (!window.grab().save(text(codeguard::utf8_path(screenshot)))) throw std::runtime_error("running screenshot failed");
                        cancel->click();
                        if (task.state() != TaskState::cancelling || cancel->isEnabled() || !task.busy())
                            throw std::runtime_error("window cancelling state failed");
                        if (window.close() || !window.isVisible()) throw std::runtime_error("window closed before safe task completion");
                        heartbeat->stop(); release->set_value();
                    } catch (const std::exception& exception) {
                        heartbeat->stop(); release->set_value(); task.cancel(); std::cerr << exception.what() << std::endl; app.exit(1);
                    }
                });
                if (!task.start(codeguard::from_utf8(bytes(smoke_project)), codeguard::from_utf8(bytes(database)), options))
                    throw std::runtime_error("window rescan did not start");
                heartbeat->start();
            } catch (const std::exception& exception) {
                std::cerr << exception.what() << std::endl;
                app.exit(1);
            }
        };
        QTimer::singleShot(0, [&]() {
            try {
                if (!scan(smoke_project) || scan(smoke_project)) throw std::runtime_error("duplicate task was accepted");
                if (open->isEnabled() || rescan->isEnabled() || reopen->isEnabled() || !filter->isEnabled() || !tree->isEnabled() || !editor->isEnabled())
                    throw std::runtime_error("busy UI capabilities are incorrect");
                filter->setText("busy UI remains interactive"); filter->clear();
            } catch (const std::exception& exception) { std::cerr << exception.what() << std::endl; task.cancel(); app.exit(1); }
        });
    } else if (!initial_project.isEmpty()) {
        QTimer::singleShot(0, [&]() {
            if (!choose_database()) return;
            try { load_config(initial_project);if(!initial_commands.isEmpty()){project_config.analysis_enabled=true;project_config.auto_discover=false;commands_path->setText(initial_commands);}scan(initial_project); }
            catch (const std::exception& exception) { QMessageBox::warning(&window, QStringLiteral("扫描未完成"), text(exception.what())); }
        });
    } else if(!session_file.isEmpty()&&database.isEmpty()) {
        QTimer::singleShot(0,[&]{try{
            QSettings session(session_file,QSettings::IniFormat);const auto saved_db=session.value("database").toString();const auto root=session.value("project").toString();
            if(root.isEmpty()||saved_db.isEmpty())return;
            codeguard::SqliteDatabase db(codeguard::from_utf8(bytes(saved_db)),true);const auto saved=db.latest(bytes(root));
            if(!saved.id)throw std::runtime_error("会话中的工程快照不存在，请重新导入工程");
            database=saved_db;load_config(root);display(saved);view_database=database;set_running(false);
            task_message->setText(QStringLiteral("已恢复上次工程与配置 · 当前显示历史快照，重新扫描可更新结果"));
        }catch(const std::exception& e){task_message->setText(QStringLiteral("会话恢复失败：")+text(e.what()));}});
    }
    // Deterministic UI acceptance: real controls, tasks and a separate restart process.
    if(!configuration_smoke.isEmpty()){
        if(configuration_smoke!="import"&&configuration_smoke!="restore")return 1;
        auto* pulse=new QTimer(&window);pulse->setInterval(50);
        QObject::connect(pulse,&QTimer::timeout,[&,pulse,stage=0,ticks=0]() mutable {
            try{
                if(++ticks>2000)throw std::runtime_error("configuration window acceptance timed out");
                if(task.busy()||build_task.busy()||current.id<=0)return;
                if(configuration_smoke=="restore"){
                    if(project_config.threads!=2||project_config.build.target!="demo"||project_config.disabled_rules!=std::vector<std::string>{"CG004"}||commands_path->text().isEmpty()||build_target->text()!="demo")
                        throw std::runtime_error("restart did not restore project controls");
                    if(current.analysis.units.size()!=1||current.build_runs.empty()||current.build_runs.front().status!="passed")throw std::runtime_error("restart did not load saved scan and build");
                    std::cout<<"GUI_CONFIG_RESTORE_OK scan="<<current.id<<std::endl;pulse->stop();app.exit(0);return;
                }
                if(stage==0){
                    ++stage;
                    QTimer::singleShot(0,[&]{
                        auto* dialog=window.findChild<QDialog*>("projectSettings");if(!dialog){app.exit(1);return;}
                        dialog->findChild<QLineEdit*>("configTarget")->setText("demo");
                        dialog->findChild<QSpinBox*>("configThreads")->setValue(2);
                        dialog->findChild<QPlainTextEdit*>("configDefinitions")->setPlainText("CG_LABEL=with spaces");
                        dialog->findChild<QPlainTextEdit*>("configIncludes")->setPlainText("resources/out");
                        dialog->findChild<QPlainTextEdit*>("configExcludes")->setPlainText("excluded");
                        dialog->findChild<QCheckBox*>("CG004")->setChecked(false);dialog->accept();
                    });
                    pulse->stop();project_settings->click();pulse->start();
                    rescan->click();return;
                }
                if(stage==1){++stage;generate->click();if(!build_task.busy())throw std::runtime_error("generate control did not start task");return;}
                if(stage==2){
                    if(current.analysis.status!="complete"||current.analysis.units.size()!=1||project_config.compile_commands.empty())throw std::runtime_error("generate did not analyze original project");
                    ++stage;build_start->click();if(!build_task.busy())throw std::runtime_error("build control did not start task");return;
                }
                if(stage==3){
                    if(build_runs.empty()||build_runs.front().status!="passed"||build_runs.front().steps.back().tests_total!=1)throw std::runtime_error("configured GUI build/test failed");
                    const auto dir=QFileInfo(database).absolutePath();
                    if(!window.grab().save(dir+"/configuration-workspace.png"))throw std::runtime_error("configuration screenshot failed");
                    ProjectSettings dialog(codeguard::from_utf8(current.root),capture_config(),&window);dialog.show();app.processEvents();
                    auto* pages=dialog.findChild<QTabWidget*>();
                    for(int i=0;i<pages->count();++i){pages->setCurrentIndex(i);app.processEvents();if(!dialog.grab().save(dir+QString("/configuration-page-%1.png").arg(i)))throw std::runtime_error("settings screenshot failed");}
                    dialog.resize(700,560);pages->setCurrentIndex(2);app.processEvents();dialog.grab().save(dir+"/configuration-compact.png");
                    remember();std::cout<<"GUI_CONFIG_IMPORT_OK scan="<<current.id<<std::endl;pulse->stop();app.exit(0);
                }
            }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;pulse->stop();app.exit(1);}
        });pulse->start();
    }
    return app.exec();
}
