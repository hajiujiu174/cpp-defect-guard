#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include "codeguard/thread_pool.hpp"
#include <gtest/gtest.h>
TEST(QueryContract, RejectsInvalidFieldsEvenWhenEmpty){
    codeguard::ScanResult scan;
    EXPECT_THROW(codeguard::execute_query(scan,"SELECT missing FROM files"),codeguard::QueryError);
    EXPECT_EQ(codeguard::execute_query(scan,"SELECT file FROM files LIMIT 0").columns,std::vector<std::string>{"file"});
}
TEST(WorkerContract, PropagatesFailureWithoutStoppingPool){
    codeguard::ThreadPool pool(2,1);
    auto bad=pool.submit([]{throw std::runtime_error("bad");});EXPECT_THROW(bad.get(),std::runtime_error);
    EXPECT_EQ(pool.submit([]{return 42;}).get(),42);
}
TEST(RuleContract, CatalogProvidesFiveExplainableRules){
    ASSERT_EQ(codeguard::rule_catalog().size(),5u);
    for(const auto& rule:codeguard::rule_catalog()){EXPECT_FALSE(rule.id.empty());EXPECT_FALSE(rule.scope.empty());}
}
