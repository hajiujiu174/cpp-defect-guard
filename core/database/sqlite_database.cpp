#include "codeguard/application.hpp"
#include <sqlite3.h>
#include <memory>
#include <stdexcept>
#include <chrono>

namespace codeguard {
namespace {
void check(int status, sqlite3* db) {
    if (status != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
void execute(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db);
}
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        check(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    void bind(int index, const std::string& value) {
        check(sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT), db_);
    }
    void bind(int index, std::int64_t value) { check(sqlite3_bind_int64(statement_, index, value), db_); }
    bool next() {
        const int status = sqlite3_step(statement_);
        if (status == SQLITE_ROW) return true;
        if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return false;
    }
    std::string text(int column) const {
        const auto value = sqlite3_column_text(statement_, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
    std::int64_t number(int column) const { return sqlite3_column_int64(statement_, column); }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};
std::int64_t scalar(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    if (!query.next()) throw std::runtime_error("expected database scalar");
    return query.number(0);
}
constexpr int application_id = 1128744018; // distinct from legacy Python run databases
} // namespace

struct SqliteDatabase::Impl {
    sqlite3* db = nullptr;
    int version = 1;
    std::shared_ptr<ScanControl> control;
    std::chrono::steady_clock::time_point wait_started;
    static int busy(void* opaque, int previous_calls) noexcept {
        auto& state = *static_cast<Impl*>(opaque);
        if (state.control && state.control->state() == ScanControl::State::cancel_requested) return 0;
        const auto now = std::chrono::steady_clock::now();
        if (!previous_calls) state.wait_started = now;
        if (now - state.wait_started >= std::chrono::seconds(5)) return 0;
        sqlite3_sleep(10);
        return 1;
    }
    ~Impl() { if (db) sqlite3_close(db); }
};
SqliteDatabase::SqliteDatabase(const fs::path& path, bool read_only, const ScanContext& context) : impl_(nullptr) {
    context.check();
    auto state = std::make_unique<Impl>();
    state->control = context.control;
    const int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    check(sqlite3_open_v2(utf8_path(path).c_str(), &state->db, flags, nullptr), state->db);
    check(sqlite3_busy_handler(state->db, &Impl::busy, state.get()), state->db);
    execute(state->db, "PRAGMA foreign_keys=ON");
    const auto id = scalar(state->db, "PRAGMA application_id");
    const auto version = scalar(state->db, "PRAGMA user_version");
    const auto tables = scalar(state->db, "SELECT count(*) FROM sqlite_master WHERE type='table'");
    if (id == 0 && version == 0 && tables == 0 && !read_only) {
        execute(state->db, R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE project(id INTEGER PRIMARY KEY, root TEXT NOT NULL UNIQUE);
CREATE TABLE scan(
 id INTEGER PRIMARY KEY, project_id INTEGER NOT NULL REFERENCES project(id),
 scanned_at TEXT NOT NULL, added INTEGER NOT NULL, changed INTEGER NOT NULL,
 unchanged INTEGER NOT NULL, removed INTEGER NOT NULL);
CREATE INDEX scan_project ON scan(project_id, id);
CREATE TABLE file(
 scan_id INTEGER NOT NULL REFERENCES scan(id), path TEXT NOT NULL,
 language TEXT NOT NULL, hash TEXT NOT NULL, size INTEGER NOT NULL CHECK(size>=0),
 mtime INTEGER NOT NULL, lines INTEGER NOT NULL CHECK(lines>=0), PRIMARY KEY(scan_id,path));
PRAGMA application_id=1128744018;
PRAGMA user_version=1;
COMMIT;
)SQL");
    } else if (id != application_id || (version < 1 || version > 5)) {
        throw std::runtime_error("not a supported CodeGuard inventory database; use a separate new database");
    }
    state->version = static_cast<int>(scalar(state->db, "PRAGMA user_version"));
    if (state->version == 1 && !read_only) {
        // Additive, transactional migration. Existing inventory snapshots stay intact.
        execute(state->db, R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE analysis(scan_id INTEGER PRIMARY KEY REFERENCES scan(id), status TEXT NOT NULL, compile_commands TEXT NOT NULL);
CREATE TABLE translation_unit(scan_id INTEGER NOT NULL REFERENCES analysis(scan_id), file TEXT NOT NULL,
 status TEXT NOT NULL, diagnostics TEXT NOT NULL, indirect_calls INTEGER NOT NULL, PRIMARY KEY(scan_id,file));
CREATE TABLE symbol(scan_id INTEGER NOT NULL REFERENCES analysis(scan_id), usr TEXT NOT NULL, kind TEXT NOT NULL,
 name TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL, col INTEGER NOT NULL,
 is_definition INTEGER NOT NULL, external INTEGER NOT NULL, PRIMARY KEY(scan_id,usr));
CREATE INDEX symbol_name ON symbol(scan_id,name);
CREATE TABLE function_metric(scan_id INTEGER NOT NULL, usr TEXT NOT NULL, lines INTEGER NOT NULL,
 parameters INTEGER NOT NULL, complexity INTEGER NOT NULL, PRIMARY KEY(scan_id,usr),
 FOREIGN KEY(scan_id,usr) REFERENCES symbol(scan_id,usr));
CREATE TABLE graph_edge(scan_id INTEGER NOT NULL REFERENCES analysis(scan_id), kind TEXT NOT NULL,
 source TEXT NOT NULL, target TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL, col INTEGER NOT NULL,
 PRIMARY KEY(scan_id,kind,source,target,file,line,col));
CREATE TABLE coverage(scan_id INTEGER NOT NULL REFERENCES analysis(scan_id), file TEXT NOT NULL, PRIMARY KEY(scan_id,file));
PRAGMA user_version=2;
COMMIT;
)SQL");
        state->version = 2;
    }
    if (state->version == 2 && !read_only) {
        execute(state->db, R"SQL(
BEGIN IMMEDIATE;
ALTER TABLE analysis ADD COLUMN workers INTEGER NOT NULL DEFAULT 0;
ALTER TABLE analysis ADD COLUMN elapsed_ms INTEGER NOT NULL DEFAULT 0;
CREATE TABLE issue(scan_id INTEGER NOT NULL REFERENCES scan(id),rule_id TEXT NOT NULL,severity TEXT NOT NULL,
 file TEXT NOT NULL,line INTEGER NOT NULL,col INTEGER NOT NULL,message TEXT NOT NULL,evidence TEXT NOT NULL,
 suggestion TEXT NOT NULL,symbol_id TEXT NOT NULL,detector TEXT NOT NULL,
 PRIMARY KEY(scan_id,rule_id,file,line,col,symbol_id));
CREATE INDEX issue_severity ON issue(scan_id,severity);
CREATE TABLE build_run(id INTEGER PRIMARY KEY,scan_id INTEGER NOT NULL REFERENCES scan(id),root TEXT NOT NULL,
 started_at TEXT NOT NULL,status TEXT NOT NULL,workspace TEXT NOT NULL,target TEXT NOT NULL,
 git_revision TEXT NOT NULL,git_log TEXT NOT NULL,source_unchanged INTEGER NOT NULL);
CREATE TABLE build_step(run_id INTEGER NOT NULL REFERENCES build_run(id),ordinal INTEGER NOT NULL,name TEXT NOT NULL,
 command TEXT NOT NULL,working_directory TEXT NOT NULL,status TEXT NOT NULL,exit_code INTEGER NOT NULL,duration_ms INTEGER NOT NULL,
 stdout TEXT NOT NULL,stderr TEXT NOT NULL,truncated INTEGER NOT NULL,tests_total INTEGER NOT NULL,tests_failed INTEGER NOT NULL,
 tests_skipped INTEGER NOT NULL,PRIMARY KEY(run_id,ordinal));
PRAGMA user_version=3;
COMMIT;
)SQL");
        state->version = 3;
    }
    if(state->version==3&&!read_only) {
        execute(state->db,R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE project_configuration(root TEXT PRIMARY KEY, payload TEXT NOT NULL);
ALTER TABLE analysis ADD COLUMN configuration TEXT NOT NULL DEFAULT '';
ALTER TABLE build_run ADD COLUMN compile_commands TEXT NOT NULL DEFAULT '';
ALTER TABLE build_run ADD COLUMN configuration TEXT NOT NULL DEFAULT '';
PRAGMA user_version=4;
COMMIT;
)SQL");
        state->version=4;
    }
    if(state->version==4&&!read_only){
        execute(state->db,R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE suppressed_issue(scan_id INTEGER NOT NULL REFERENCES scan(id),rule_id TEXT NOT NULL,severity TEXT NOT NULL,
 file TEXT NOT NULL,line INTEGER NOT NULL,col INTEGER NOT NULL,message TEXT NOT NULL,evidence TEXT NOT NULL,
 suggestion TEXT NOT NULL,symbol_id TEXT NOT NULL,detector TEXT NOT NULL,reason TEXT NOT NULL,
 PRIMARY KEY(scan_id,rule_id,file,line,col,symbol_id));
CREATE TABLE rule_diagnostic(scan_id INTEGER NOT NULL REFERENCES scan(id),ordinal INTEGER NOT NULL,message TEXT NOT NULL,
 PRIMARY KEY(scan_id,ordinal));
PRAGMA user_version=5;
COMMIT;
)SQL");state->version=5;
    }
    impl_ = state.release();
}
SqliteDatabase::~SqliteDatabase() { delete impl_; }

std::optional<ProjectConfig> SqliteDatabase::configuration(const std::string& root) {
    if(impl_->version<4)return {};
    Statement query(impl_->db,"SELECT payload FROM project_configuration WHERE root=?");query.bind(1,root);
    if(!query.next())return {};
    return decode_config(query.text(0));
}
void SqliteDatabase::save_configuration(const std::string& root,const ProjectConfig& config) {
    const auto payload=encode_config(config);
    (void)configuration(root); // never silently replace an unsupported/corrupt profile
    Statement write(impl_->db,"INSERT INTO project_configuration(root,payload) VALUES(?,?) ON CONFLICT(root) DO UPDATE SET payload=excluded.payload");
    write.bind(1,root);write.bind(2,payload);write.next();
}

ScanResult SqliteDatabase::latest(const std::string& root) {
    ScanResult result;
    result.root = root;
    Statement scan(impl_->db, "SELECT s.id,s.scanned_at,s.added,s.changed,s.unchanged,s.removed FROM scan s JOIN project p ON p.id=s.project_id WHERE p.root=? ORDER BY s.id DESC LIMIT 1");
    scan.bind(1, root);
    if (!scan.next()) return result;
    result.id = scan.number(0);
    result.scanned_at = scan.text(1);
    result.added = scan.number(2);
    result.changed = scan.number(3);
    result.unchanged = scan.number(4);
    result.removed = scan.number(5);
    Statement files(impl_->db, "SELECT path,language,hash,size,mtime,lines FROM file WHERE scan_id=? ORDER BY path");
    files.bind(1, result.id);
    while (files.next()) {
        FileRecord file;
        file.path = files.text(0); file.language = files.text(1); file.hash = files.text(2);
        file.size = files.number(3); file.mtime = files.number(4); file.lines = files.number(5);
        result.files.push_back(file);
    }
    if (impl_->version >= 2) {
        auto& output = result.analysis;
        Statement analysis(impl_->db, "SELECT status,compile_commands FROM analysis WHERE scan_id=?");
        analysis.bind(1, result.id);
        if (analysis.next()) { output.status = analysis.text(0); output.compile_commands = analysis.text(1); }
        if(impl_->version>=4) {
            Statement settings(impl_->db,"SELECT configuration FROM analysis WHERE scan_id=?");settings.bind(1,result.id);
            if(settings.next())output.configuration=settings.text(0);
        }
        if(impl_->version>=5){
            Statement suppressed(impl_->db,"SELECT rule_id,severity,file,line,col,message,evidence,suggestion,symbol_id,detector,reason FROM suppressed_issue WHERE scan_id=? ORDER BY file,line,col,rule_id,symbol_id");
            suppressed.bind(1,result.id);
            while(suppressed.next())output.suppressed_issues.push_back({suppressed.text(0),suppressed.text(1),suppressed.text(2),static_cast<int>(suppressed.number(3)),static_cast<int>(suppressed.number(4)),suppressed.text(5),suppressed.text(6),suppressed.text(7),suppressed.text(8),suppressed.text(9),suppressed.text(10)});
            Statement diagnostic(impl_->db,"SELECT message FROM rule_diagnostic WHERE scan_id=? ORDER BY ordinal");diagnostic.bind(1,result.id);
            while(diagnostic.next())output.rule_diagnostics.push_back(diagnostic.text(0));
        }
        Statement units(impl_->db, "SELECT file,status,diagnostics,indirect_calls FROM translation_unit WHERE scan_id=? ORDER BY file");
        units.bind(1, result.id);
        while (units.next()) output.units.push_back({units.text(0), units.text(1), units.text(2), static_cast<int>(units.number(3))});
        Statement symbols(impl_->db, "SELECT usr,kind,name,file,line,col,is_definition,external FROM symbol WHERE scan_id=? ORDER BY usr");
        symbols.bind(1, result.id);
        while (symbols.next()) output.symbols.push_back({symbols.text(0), symbols.text(1), symbols.text(2), symbols.text(3),
            static_cast<int>(symbols.number(4)), static_cast<int>(symbols.number(5)), symbols.number(6) != 0, symbols.number(7) != 0});
        Statement metrics(impl_->db, "SELECT usr,lines,parameters,complexity FROM function_metric WHERE scan_id=? ORDER BY usr");
        metrics.bind(1, result.id);
        while (metrics.next()) output.metrics.push_back({metrics.text(0), static_cast<int>(metrics.number(1)),
            static_cast<int>(metrics.number(2)), static_cast<int>(metrics.number(3))});
        Statement edges(impl_->db, "SELECT kind,source,target,file,line,col FROM graph_edge WHERE scan_id=? ORDER BY kind,source,target,file,line,col");
        edges.bind(1, result.id);
        while (edges.next()) output.edges.push_back({edges.text(0), edges.text(1), edges.text(2), edges.text(3),
            static_cast<int>(edges.number(4)), static_cast<int>(edges.number(5))});
        Statement coverage(impl_->db, "SELECT file FROM coverage WHERE scan_id=? ORDER BY file");
        coverage.bind(1, result.id);
        while (coverage.next()) output.covered_files.push_back(coverage.text(0));
        if (impl_->version >= 3) {
            Statement stats(impl_->db, "SELECT workers,elapsed_ms FROM analysis WHERE scan_id=?"); stats.bind(1, result.id);
            if (stats.next()) { output.workers = static_cast<unsigned>(stats.number(0)); output.elapsed_ms = stats.number(1); }
            Statement issues(impl_->db, "SELECT rule_id,severity,file,line,col,message,evidence,suggestion,symbol_id,detector FROM issue WHERE scan_id=? ORDER BY file,line,col,rule_id,symbol_id");
            issues.bind(1, result.id);
            while (issues.next()) output.issues.push_back({issues.text(0),issues.text(1),issues.text(2),static_cast<int>(issues.number(3)),
                static_cast<int>(issues.number(4)),issues.text(5),issues.text(6),issues.text(7),issues.text(8),issues.text(9)});
        }
    }
    if (impl_->version >= 3) for (auto& run : builds(root)) if (run.scan_id == result.id) result.build_runs.push_back(std::move(run));
    return result;
}
std::vector<ProjectSummary> SqliteDatabase::projects() {
    Statement query(impl_->db, "SELECT p.root,s.scanned_at FROM project p JOIN scan s ON s.id=(SELECT MAX(id) FROM scan WHERE project_id=p.id) ORDER BY s.id DESC");
    std::vector<ProjectSummary> result;
    while (query.next()) result.push_back({query.text(0), query.text(1)});
    return result;
}
void SqliteDatabase::save(ScanResult& result, const ScanContext& context) {
    context.check();
    if (!result.diagnostics.empty()) throw std::invalid_argument("cannot persist incomplete inventory");
    if (result.root.empty()) throw std::invalid_argument("project root is empty");
    auto* db = impl_->db;
    // A save-specific token may differ from the connection's read/init token.
    struct RestoreControl {
        Impl& state;
        std::shared_ptr<ScanControl> previous;
        ~RestoreControl() { state.control = std::move(previous); }
    } restore{*impl_, impl_->control};
    if (context.control) impl_->control = context.control;
    try {
        execute(db, "BEGIN IMMEDIATE");
        Statement project(db, "INSERT INTO project(root) VALUES(?) ON CONFLICT(root) DO NOTHING");
        project.bind(1, result.root); project.next();
        Statement scan(db, "INSERT INTO scan(project_id,scanned_at,added,changed,unchanged,removed) SELECT id,?,?,?,?,? FROM project WHERE root=?");
        scan.bind(1, result.scanned_at);
        scan.bind(2, static_cast<std::int64_t>(result.added));
        scan.bind(3, static_cast<std::int64_t>(result.changed));
        scan.bind(4, static_cast<std::int64_t>(result.unchanged));
        scan.bind(5, static_cast<std::int64_t>(result.removed));
        scan.bind(6, result.root); scan.next();
        const auto id = sqlite3_last_insert_rowid(db);
        for (const auto& file : result.files) {
            context.check();
            Statement insert(db, "INSERT INTO file(scan_id,path,language,hash,size,mtime,lines) VALUES(?,?,?,?,?,?,?)");
            insert.bind(1, id); insert.bind(2, file.path); insert.bind(3, file.language);
            insert.bind(4, file.hash); insert.bind(5, static_cast<std::int64_t>(file.size));
            insert.bind(6, file.mtime); insert.bind(7, static_cast<std::int64_t>(file.lines));
            insert.next();
        }
        const auto& data = result.analysis;
        Statement analysis(db, "INSERT INTO analysis(scan_id,status,compile_commands,workers,elapsed_ms,configuration) VALUES(?,?,?,?,?,?)");
        analysis.bind(1, id); analysis.bind(2, data.status); analysis.bind(3, data.compile_commands);
        analysis.bind(4, data.workers); analysis.bind(5, data.elapsed_ms); analysis.bind(6,data.configuration); analysis.next();
        for (const auto& unit : data.units) {
            context.check();
            Statement row(db, "INSERT INTO translation_unit VALUES(?,?,?,?,?)");
            row.bind(1, id); row.bind(2, unit.file); row.bind(3, unit.status); row.bind(4, unit.diagnostics);
            row.bind(5, unit.indirect_calls); row.next();
        }
        for (const auto& symbol : data.symbols) {
            context.check();
            Statement row(db, "INSERT INTO symbol VALUES(?,?,?,?,?,?,?,?,?)");
            row.bind(1, id); row.bind(2, symbol.id); row.bind(3, symbol.kind); row.bind(4, symbol.name);
            row.bind(5, symbol.file); row.bind(6, symbol.line); row.bind(7, symbol.column);
            row.bind(8, symbol.definition ? 1 : 0); row.bind(9, symbol.external ? 1 : 0); row.next();
        }
        for (const auto& metric : data.metrics) {
            context.check();
            Statement row(db, "INSERT INTO function_metric VALUES(?,?,?,?,?)");
            row.bind(1, id); row.bind(2, metric.symbol_id); row.bind(3, metric.lines);
            row.bind(4, metric.parameters); row.bind(5, metric.complexity); row.next();
        }
        for (const auto& edge : data.edges) {
            context.check();
            Statement row(db, "INSERT INTO graph_edge VALUES(?,?,?,?,?,?,?)");
            row.bind(1, id); row.bind(2, edge.kind); row.bind(3, edge.source); row.bind(4, edge.target);
            row.bind(5, edge.file); row.bind(6, edge.line); row.bind(7, edge.column); row.next();
        }
        for (const auto& file : data.covered_files) {
            context.check();
            Statement row(db, "INSERT INTO coverage VALUES(?,?)");
            row.bind(1, id); row.bind(2, file); row.next();
        }
        for(const auto* list:{&data.issues,&data.suppressed_issues})for (const auto& issue : *list) {
            context.check(); Statement row(db, list==&data.issues?"INSERT INTO issue VALUES(?,?,?,?,?,?,?,?,?,?,?)":"INSERT INTO suppressed_issue VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
            row.bind(1,id); row.bind(2,issue.rule_id); row.bind(3,issue.severity); row.bind(4,issue.file);
            row.bind(5,issue.line); row.bind(6,issue.column); row.bind(7,issue.message); row.bind(8,issue.evidence);
            row.bind(9,issue.suggestion); row.bind(10,issue.symbol_id); row.bind(11,issue.detector);
            if(list==&data.suppressed_issues)row.bind(12,issue.suppression_reason);row.next();
        }
        for(std::size_t n=0;n<data.rule_diagnostics.size();++n){context.check();Statement row(db,"INSERT INTO rule_diagnostic VALUES(?,?,?)");row.bind(1,id);row.bind(2,static_cast<std::int64_t>(n));row.bind(3,data.rule_diagnostics[n]);row.next();}
        context.report("before_commit");
        if (context.control) context.control->begin_commit();
        execute(db, "COMMIT");
        result.id = id;
    } catch (...) {
        if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        context.check();
        throw;
    }
}
void SqliteDatabase::save_build(BuildRun& run) {
    auto* db = impl_->db;
    if (run.scan_id <= 0 || run.root.empty() || run.steps.empty()) throw std::invalid_argument("build run requires scan, root and steps");
    try {
        execute(db, "BEGIN IMMEDIATE");
        Statement verify(db, "SELECT 1 FROM scan s JOIN project p ON p.id=s.project_id WHERE s.id=? AND p.root=?");
        verify.bind(1,run.scan_id); verify.bind(2,run.root);
        if (!verify.next()) throw std::invalid_argument("build scan/project mismatch");
        Statement row(db,"INSERT INTO build_run(scan_id,root,started_at,status,workspace,target,git_revision,git_log,source_unchanged) VALUES(?,?,?,?,?,?,?,?,?)");
        row.bind(1,run.scan_id); row.bind(2,run.root); row.bind(3,run.started_at); row.bind(4,run.status);
        row.bind(5,run.workspace); row.bind(6,run.target); row.bind(7,run.git_revision); row.bind(8,run.git_log); row.bind(9,run.source_unchanged ? 1 : 0); row.next();
        const auto id = sqlite3_last_insert_rowid(db); std::int64_t ordinal = 0;
        Statement settings(db,"UPDATE build_run SET compile_commands=?,configuration=? WHERE id=?");settings.bind(1,run.compile_commands);settings.bind(2,run.configuration);settings.bind(3,id);settings.next();
        for (const auto& step : run.steps) {
            Statement item(db,"INSERT INTO build_step VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
            item.bind(1,id); item.bind(2,ordinal++); item.bind(3,step.name); item.bind(4,step.command); item.bind(5,step.working_directory);
            item.bind(6,step.result.status); item.bind(7,step.result.exit_code); item.bind(8,step.result.duration_ms);
            item.bind(9,step.result.stdout_text); item.bind(10,step.result.stderr_text); item.bind(11,step.result.output_truncated ? 1 : 0);
            item.bind(12,step.tests_total); item.bind(13,step.tests_failed); item.bind(14,step.tests_skipped); item.next();
        }
        execute(db,"COMMIT"); run.id = id;
    } catch (...) { if (!sqlite3_get_autocommit(db)) sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
}
std::vector<BuildRun> SqliteDatabase::builds(const std::string& root, bool include_logs, std::int64_t only_id) {
    std::vector<BuildRun> result;
    if (impl_->version < 3) return result;
    Statement query(impl_->db,"SELECT id,scan_id,root,started_at,status,workspace,target,git_revision,CASE WHEN ? THEN git_log ELSE '' END,source_unchanged FROM build_run WHERE root=? AND (?=0 OR id=?) ORDER BY id DESC LIMIT 50");
    query.bind(1,include_logs ? 1 : 0);query.bind(2,root);query.bind(3,only_id);query.bind(4,only_id);
    while (query.next()) {
        BuildRun run; run.id=query.number(0); run.scan_id=query.number(1); run.root=query.text(2); run.started_at=query.text(3);
        run.status=query.text(4); run.workspace=query.text(5); run.target=query.text(6); run.git_revision=query.text(7); run.git_log=query.text(8); run.source_unchanged=query.number(9)!=0;
        if(impl_->version>=4) {
            Statement settings(impl_->db,"SELECT compile_commands,configuration FROM build_run WHERE id=?");settings.bind(1,run.id);
            if(settings.next()){run.compile_commands=settings.text(0);run.configuration=settings.text(1);}
        }
        Statement steps(impl_->db,"SELECT name,command,working_directory,status,exit_code,duration_ms,CASE WHEN ? THEN stdout ELSE '' END,CASE WHEN ? THEN stderr ELSE '' END,truncated,tests_total,tests_failed,tests_skipped FROM build_step WHERE run_id=? ORDER BY ordinal");
        steps.bind(1,include_logs ? 1 : 0);steps.bind(2,include_logs ? 1 : 0);steps.bind(3,run.id);
        while (steps.next()) {
            BuildStep step; step.name=steps.text(0); step.command=steps.text(1); step.working_directory=steps.text(2);
            step.result.status=steps.text(3); step.result.exit_code=static_cast<int>(steps.number(4)); step.result.duration_ms=steps.number(5);
            step.result.stdout_text=steps.text(6); step.result.stderr_text=steps.text(7); step.result.output_truncated=steps.number(8)!=0;
            step.tests_total=static_cast<int>(steps.number(9)); step.tests_failed=static_cast<int>(steps.number(10)); step.tests_skipped=static_cast<int>(steps.number(11));
            run.steps.push_back(std::move(step));
        }
        result.push_back(std::move(run));
    }
    return result;
}
} // namespace codeguard
