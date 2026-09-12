#include "codeguard/query.hpp"
#include "codeguard/application.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>

namespace codeguard {
namespace {
using Kind = QueryToken::Kind;
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
std::string error_text(const std::string& source, std::size_t offset, const std::string& message) {
    std::size_t line = 1, column = 1;
    for (std::size_t i = 0; i < std::min(offset, source.size()); ++i)
        if (source[i] == '\n') { ++line; column = 1; } else ++column;
    return "query line " + std::to_string(line) + ", byte column " + std::to_string(column) + ": " + message;
}
std::int64_t integer(const std::string& source, const QueryToken& token) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
    if (error != std::errc{} || end != token.text.data() + token.text.size())
        throw QueryError(source, token.offset, "integer outside signed 64-bit range");
    return value;
}
class Parser {
    const std::string& source_;
    std::vector<QueryToken> tokens_;
    std::size_t position_ = 0;
    QueryAst ast_;
    const QueryToken& peek() const { return tokens_[position_]; }
    bool take(Kind kind) { if (peek().kind != kind) return false; ++position_; return true; }
    bool word(const std::string& value) {
        if (peek().kind != Kind::word || peek().text != value) return false;
        ++position_; return true;
    }
    void require(const std::string& value) {
        if (!word(value)) throw QueryError(source_, peek().offset, "expected " + value);
    }
    QueryToken field() {
        if (peek().kind != Kind::word) throw QueryError(source_, peek().offset, "expected identifier");
        return tokens_[position_++];
    }
    int append(QueryExpression expression) {
        if (ast_.expressions.size() >= 256) throw QueryError(source_, peek().offset, "expression exceeds 256 nodes");
        ast_.expressions.push_back(std::move(expression));
        return static_cast<int>(ast_.expressions.size()) - 1;
    }
    int primary(int depth) {
        if (depth > 64) throw QueryError(source_, peek().offset, "parentheses exceed depth 64");
        if (take(Kind::left)) {
            const int root = disjunction(depth + 1);
            if (!take(Kind::right)) throw QueryError(source_, peek().offset, "expected )");
            return root;
        }
        const auto name = field();
        QueryExpression e; e.field = name.text; e.offset = name.offset;
        if (peek().kind != Kind::op) throw QueryError(source_, peek().offset, "expected comparison operator");
        e.operation = tokens_[position_++].text;
        if (peek().kind == Kind::number) e.value = integer(source_, tokens_[position_++]);
        else if (peek().kind == Kind::string) e.value = tokens_[position_++].text;
        else throw QueryError(source_, peek().offset, "expected integer or single-quoted string");
        return append(std::move(e));
    }
    int conjunction(int depth) {
        int left = primary(depth);
        while (word("and")) {
            const int right = primary(depth);
            QueryExpression e; e.kind = QueryExpression::Kind::conjunction; e.left = left; e.right = right;
            left = append(std::move(e));
        }
        return left;
    }
    int disjunction(int depth) {
        int left = conjunction(depth);
        while (word("or")) {
            const int right = conjunction(depth);
            QueryExpression e; e.kind = QueryExpression::Kind::disjunction; e.left = left; e.right = right;
            left = append(std::move(e));
        }
        return left;
    }
public:
    explicit Parser(const std::string& source) : source_(source), tokens_(lex_query(source)) {}
    QueryAst parse() {
        require("select");
        if (peek().kind == Kind::star) ast_.columns.push_back(tokens_[position_++]);
        else { do { ast_.columns.push_back(field()); } while (take(Kind::comma)); }
        require("from"); const auto table = field(); ast_.table = table.text; ast_.table_offset = table.offset;
        if (word("where")) ast_.predicate = disjunction(0);
        if (word("order")) {
            require("by");
            do {
                const auto name = field(); QueryOrder order{name.text, false, name.offset};
                if (word("desc")) order.descending = true; else word("asc");
                ast_.order.push_back(order);
            } while (take(Kind::comma));
        }
        if (word("limit")) {
            const auto token = peek();
            if (!take(Kind::number)) throw QueryError(source_, token.offset, "expected non-negative LIMIT integer");
            ast_.limit = integer(source_, token);
            if (ast_.limit < 0) throw QueryError(source_, token.offset, "LIMIT must be non-negative");
        }
        take(Kind::semicolon);
        if (peek().kind != Kind::end) throw QueryError(source_, peek().offset, "unexpected input; only one SELECT is allowed");
        return std::move(ast_);
    }
};
struct Column { std::string name; bool number; };
using Schema = std::vector<Column>;
const std::map<std::string, Schema> schemas = {
    {"suppressed_issues", {{"rule_id",false},{"severity",false},{"file",false},{"line",true},{"column",true},{"message",false},{"evidence",false},{"suggestion",false},{"symbol_id",false},{"detector",false},{"reason",false}}},
    {"rule_diagnostics", {{"message",false}}},
    {"issues", {{"rule_id",false},{"severity",false},{"file",false},{"line",true},{"column",true},{"message",false},{"evidence",false},{"suggestion",false},{"symbol_id",false},{"detector",false}}},
    {"builds", {{"run_id",true},{"scan_id",true},{"stage",false},{"status",false},{"exit_code",true},{"duration_ms",true},{"tests_total",true},{"tests_failed",true},{"tests_skipped",true},{"target",false}}},
    {"files", {{"file",false},{"language",false},{"lines",true},{"size",true},{"mtime",true},{"hash",false}}},
    {"functions", {{"name",false},{"file",false},{"line",true},{"complexity",true},{"lines",true},{"parameters",true},{"symbol_id",false}}},
    {"symbols", {{"name",false},{"file",false},{"line",true},{"column",true},{"kind",false},{"definition",true},{"external",true},{"symbol_id",false}}},
    {"edges", {{"kind",false},{"source",false},{"target",false},{"file",false},{"line",true},{"column",true}}}
};
std::size_t field_index(const Schema& schema, const std::string& field, const std::string& source, std::size_t offset) {
    for (std::size_t i = 0; i < schema.size(); ++i) if (schema[i].name == field) return i;
    throw QueryError(source, offset, "unknown field: " + field);
}
struct Plan {
    Schema schema;
    std::vector<std::size_t> projection, predicate_fields, order_fields;
};
Plan analyze(const QueryAst& ast, const std::string& source) {
    const auto found = schemas.find(ast.table);
    if (found == schemas.end()) throw QueryError(source, ast.table_offset, "unknown table: " + ast.table);
    Plan plan; plan.schema = found->second;
    if (ast.columns.front().kind == Kind::star) {
        for (std::size_t i = 0; i < plan.schema.size(); ++i) plan.projection.push_back(i);
    } else for (const auto& column : ast.columns)
        plan.projection.push_back(field_index(plan.schema, column.text, source, column.offset));
    for (const auto& e : ast.expressions) {
        std::size_t index = 0;
        if (e.kind == QueryExpression::Kind::comparison) {
            index = field_index(plan.schema, e.field, source, e.offset);
            if (plan.schema[index].number != std::holds_alternative<std::int64_t>(e.value))
                throw QueryError(source, e.offset, "type mismatch for field: " + e.field);
        }
        plan.predicate_fields.push_back(index);
    }
    for (const auto& order : ast.order) plan.order_fields.push_back(field_index(plan.schema, order.field, source, order.offset));
    return plan;
}
std::int64_t signed_size(std::uintmax_t value) {
    if (value > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("scan value exceeds query integer range");
    return static_cast<std::int64_t>(value);
}
using Row = std::vector<QueryValue>;
std::vector<Row> materialize(const ScanResult& scan, const std::string& table) {
    std::vector<Row> rows;
    if(table=="suppressed_issues") {
        for(const auto& i:scan.analysis.suppressed_issues)rows.push_back({i.rule_id,i.severity,i.file,std::int64_t{i.line},std::int64_t{i.column},i.message,i.evidence,i.suggestion,i.symbol_id,i.detector,i.suppression_reason});
    }else if(table=="rule_diagnostics"){
        for(const auto& message:scan.analysis.rule_diagnostics)rows.push_back({message});
    }else if (table == "issues") {
        for (const auto& i : scan.analysis.issues) rows.push_back({i.rule_id,i.severity,i.file,std::int64_t{i.line},std::int64_t{i.column},i.message,i.evidence,i.suggestion,i.symbol_id,i.detector});
    } else if (table == "builds") {
        for (const auto& run : scan.build_runs) for (const auto& step : run.steps) rows.push_back({run.id,run.scan_id,step.name,step.result.status,
            std::int64_t{step.result.exit_code},step.result.duration_ms,std::int64_t{step.tests_total},std::int64_t{step.tests_failed},std::int64_t{step.tests_skipped},run.target});
    } else if (table == "files") {
        for (const auto& f : scan.files) rows.push_back({f.path, f.language, signed_size(f.lines), signed_size(f.size), f.mtime, f.hash});
    } else if (table == "symbols") {
        for (const auto& s : scan.analysis.symbols) rows.push_back({s.name,s.file,std::int64_t{s.line},std::int64_t{s.column},s.kind,std::int64_t{s.definition},std::int64_t{s.external},s.id});
    } else if (table == "edges") {
        for (const auto& e : scan.analysis.edges) rows.push_back({e.kind,e.source,e.target,e.file,std::int64_t{e.line},std::int64_t{e.column}});
    } else {
        std::map<std::string, const Symbol*> symbols;
        for (const auto& s : scan.analysis.symbols) symbols[s.id] = &s;
        for (const auto& m : scan.analysis.metrics) {
            const auto found = symbols.find(m.symbol_id);
            if (found == symbols.end()) throw std::runtime_error("metric refers to a missing symbol");
            const auto& s = *found->second;
            rows.push_back({s.name,s.file,std::int64_t{s.line},std::int64_t{m.complexity},std::int64_t{m.lines},std::int64_t{m.parameters},s.id});
        }
    }
    return rows;
}
bool matches(const Row& row, const QueryAst& ast, const Plan& plan, int node) {
    if (node < 0) return true;
    const auto& e = ast.expressions[static_cast<std::size_t>(node)];
    if (e.kind == QueryExpression::Kind::conjunction) return matches(row, ast, plan, e.left) && matches(row, ast, plan, e.right);
    if (e.kind == QueryExpression::Kind::disjunction) return matches(row, ast, plan, e.left) || matches(row, ast, plan, e.right);
    const auto& value = row[plan.predicate_fields[static_cast<std::size_t>(node)]];
    if (e.operation == "=") return value == e.value;
    if (e.operation == "!=" || e.operation == "<>") return value != e.value;
    if (e.operation == "<") return value < e.value;
    if (e.operation == "<=") return value <= e.value;
    if (e.operation == ">") return value > e.value;
    return value >= e.value;
}
} // namespace
QueryError::QueryError(const std::string& source, std::size_t at, const std::string& message)
    : std::runtime_error(error_text(source, at, message)), offset(at) {}
std::vector<QueryToken> lex_query(const std::string& source) {
    if (source.size() > 65536) throw QueryError(source, 65536, "query exceeds 64 KiB");
    std::vector<QueryToken> tokens;
    for (std::size_t i = 0; i < source.size();) {
        const auto start = i; const char c = source[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (tokens.size() >= 4096) throw QueryError(source, i, "query exceeds 4096 tokens");
        if (letter(c)) {
            while (i < source.size() && (letter(source[i]) || digit(source[i]))) ++i;
            tokens.push_back({Kind::word, lower(source.substr(start, i - start)), start});
        } else if (digit(c) || (c == '-' && i + 1 < source.size() && digit(source[i + 1]))) {
            ++i; while (i < source.size() && digit(source[i])) ++i;
            tokens.push_back({Kind::number, source.substr(start, i - start), start});
        } else if (c == '\'') {
            ++i; std::string value; bool closed = false;
            while (i < source.size()) {
                if (source[i] == '\'') {
                    ++i;
                    if (i < source.size() && source[i] == '\'') { value += '\''; ++i; }
                    else { closed = true; break; }
                } else value += source[i++];
            }
            if (!closed) throw QueryError(source, start, "unterminated string");
            tokens.push_back({Kind::string, value, start});
        } else {
            ++i; Kind kind;
            if (c == ',') kind = Kind::comma;
            else if (c == '*') kind = Kind::star;
            else if (c == '(') kind = Kind::left;
            else if (c == ')') kind = Kind::right;
            else if (c == ';') kind = Kind::semicolon;
            else if (c == '=' || c == '<' || c == '>' || c == '!') {
                kind = Kind::op;
                if (i < source.size() && ((source[i] == '=' && c != '=') || (c == '<' && source[i] == '>'))) ++i;
                if (c == '!' && i == start + 1) throw QueryError(source, start, "expected !=");
            } else throw QueryError(source, start, "unsupported character");
            tokens.push_back({kind, source.substr(start, i - start), start});
        }
    }
    tokens.push_back({Kind::end, "", source.size()});
    return tokens;
}
QueryAst parse_query(const std::string& source) { return Parser(source).parse(); }
std::string query_value_text(const QueryValue& value) {
    if (const auto* number = std::get_if<std::int64_t>(&value)) return std::to_string(*number);
    return std::get<std::string>(value);
}
QueryResult execute_query(const ScanResult& scan, const std::string& source) {
    const auto ast = parse_query(source);
    const auto plan = analyze(ast, source); // validate even on empty input
    if (ast.table != "files" && ast.table != "builds" && ast.table != "rule_diagnostics" && scan.analysis.status == "not_requested")
        throw QueryError(source, ast.table_offset, "table requires a Clang analysis snapshot");
    const auto data = materialize(scan, ast.table);
    QueryResult result; result.analysis_status = scan.analysis.status; result.scan_id = scan.id; result.scanned_rows = data.size();
    for (const auto index : plan.projection) result.columns.push_back(plan.schema[index].name);
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < data.size(); ++i) if (matches(data[i], ast, plan, ast.predicate)) selected.push_back(i);
    result.matched_rows = selected.size();
    if (!ast.order.empty()) std::stable_sort(selected.begin(), selected.end(), [&](auto a, auto b) {
        for (std::size_t i = 0; i < ast.order.size(); ++i) {
            const auto& left = data[a][plan.order_fields[i]]; const auto& right = data[b][plan.order_fields[i]];
            if (left != right) return ast.order[i].descending ? left > right : left < right;
        }
        return false;
    });
    if (ast.limit >= 0 && static_cast<std::uint64_t>(ast.limit) < selected.size()) selected.resize(static_cast<std::size_t>(ast.limit));
    for (const auto i : selected) {
        Row row; for (const auto index : plan.projection) row.push_back(data[i][index]);
        result.rows.push_back(std::move(row));
    }
    std::ostringstream description;
    description << "Snapshot(scan=" << scan.id << ", analysis=" << scan.analysis.status << ") -> Scan(" << ast.table << ")";
    if (ast.predicate >= 0) description << " -> Filter(AST nodes=" << ast.expressions.size() << ")";
    if (!ast.order.empty()) {
        description << " -> StableSort(";
        for (std::size_t i = 0; i < ast.order.size(); ++i) { if (i) description << ", "; description << ast.order[i].field << (ast.order[i].descending ? " DESC" : " ASC"); }
        description << ')';
    }
    if (ast.limit >= 0) description << " -> Limit(" << ast.limit << ')';
    description << " -> Project(";
    for (std::size_t i = 0; i < result.columns.size(); ++i) { if (i) description << ", "; description << result.columns[i]; }
    description << ')'; result.plan = description.str();
    return result;
}
} // namespace codeguard
