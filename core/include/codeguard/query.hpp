#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace codeguard {
struct ScanResult;
using QueryValue = std::variant<std::int64_t, std::string>;
struct QueryToken {
    enum class Kind { word, number, string, comma, star, left, right, op, semicolon, end };
    Kind kind;
    std::string text;
    std::size_t offset;
};
struct QueryExpression {
    enum class Kind { comparison, conjunction, disjunction };
    Kind kind = Kind::comparison;
    std::string field, operation;
    QueryValue value = std::int64_t{0};
    int left = -1, right = -1;
    std::size_t offset = 0;
};
struct QueryOrder { std::string field; bool descending = false; std::size_t offset = 0; };
struct QueryAst {
    std::string table;
    std::size_t table_offset = 0;
    std::vector<QueryToken> columns;
    std::vector<QueryExpression> expressions;
    int predicate = -1;
    std::vector<QueryOrder> order;
    std::int64_t limit = -1; // no LIMIT
};
class QueryError : public std::runtime_error {
public:
    QueryError(const std::string& source, std::size_t offset, const std::string& message);
    std::size_t offset;
};
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<QueryValue>> rows;
    std::string plan;
    std::string analysis_status;
    std::int64_t scan_id = 0;
    std::size_t scanned_rows = 0, matched_rows = 0;
};
// Pure C++ language pipeline. It never sends query text to SQLite.
std::vector<QueryToken> lex_query(const std::string& source);
QueryAst parse_query(const std::string& source);
QueryResult execute_query(const ScanResult& scan, const std::string& source);
std::string query_value_text(const QueryValue& value);
} // namespace codeguard
