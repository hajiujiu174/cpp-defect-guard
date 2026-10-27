#pragma once
#include "codeguard/repair.hpp"
#include <QString>
#include <QStringList>

inline QString repairTestCoverageText(const codeguard::RepairTestCoverage& coverage){
    if(coverage.status=="not_measured")return {};
    auto text=[](const std::string& s){return QString::fromUtf8(s.data(),static_cast<qsizetype>(s.size()));};
    QString result=QStringLiteral("逐测试组覆盖：%1；行记录并集与整套测试一致：%2\n测试组包含依赖和自动 fixture；每组重置计数，沿用同一构建环境。\n%3\n").arg(text(coverage.status),coverage.line_union_matches_aggregate?QStringLiteral("是"):QStringLiteral("未确认"),text(coverage.error));
    for(const auto& entry:coverage.entries){QStringList selected;for(const auto& name:entry.selected_tests)selected.append(text(name));result+=QStringLiteral("请求：%1；状态：%2；本组：%3\n").arg(text(entry.name),text(entry.status),selected.join(QStringLiteral("，")));
        for(const auto& target:entry.targets)result+=QStringLiteral("  %1:%2：%3\n").arg(text(target.file)).arg(target.line).arg(text(target.status));
        if(!entry.error.empty())result+=text(entry.error)+'\n';
    }return result+QStringLiteral("逐测试记录：%1/summary.json").arg(text(coverage.workspace));
}
