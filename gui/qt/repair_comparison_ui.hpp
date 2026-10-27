#pragma once
#include "codeguard/repair_comparison.hpp"
#include <QString>

inline QString repairComparisonText(const codeguard::RepairCoverageComparison& r){
    auto q=[](const std::string& s){return QString::fromUtf8(s.data(),static_cast<qsizetype>(s.size()));};
    auto yes=[](bool b){return b?QStringLiteral("是"):QStringLiteral("未确认");};
    QString text=QStringLiteral("多轮覆盖对照：%1\n每个版本 %2 轮；修复正确性：未评估\n完整对照：%3；覆盖稳定：%4；测试结果稳定：%5\n测试目录一致：%6；工具版本一致：%7；副本材料核对：%8\n补丁标准验证全部通过：%9\n原始 C/C++ 内容一致：%10；已知原始输入一致：%11\n完整记录：%12/comparison.json\n%13\n")
        .arg(q(r.status)).arg(r.rounds).arg(yes(r.comparison_complete),yes(r.coverage_stable),yes(r.tests_stable),yes(r.catalogs_match),yes(r.tools_match),yes(r.all_materials_verified),yes(r.all_patched_validations_passed),yes(r.original_cpp_unchanged),yes(r.known_original_inputs_unchanged),q(r.workspace),q(r.error).left(8192));
    constexpr qsizetype limit=131072;unsigned rows=0;
    auto append=[&](const QString& s){if(text.size()<limit)text+=s.left(limit-text.size());};
    for(const auto& o:r.observations)append(QStringLiteral("第 %1 轮 %2：%3；材料 %4；标准验证 %5\n%6\n").arg(o.round+1).arg(q(o.version),q(o.status),yes(o.material_verified),yes(o.standard_validation_passed),q(o.error).left(2048)));
    append(QStringLiteral("仅未修改且有明确整行对应关系的行比较执行记录；修改行单列。原版失败测试组仅作诊断。\n"));
    for(const auto& g:r.groups){if(rows>=500||text.size()>=limit)break;append(QStringLiteral("组：%1%2\n").arg(g.name.empty()?QStringLiteral("整套测试"):q(g.name).left(2048),g.diagnostic_only?QStringLiteral("（原版测试失败，诊断记录）"):QString{}));
        for(const auto& f:g.files){if(rows>=500||text.size()>=limit)break;for(const auto& l:f.lines){if(rows>=500||text.size()>=limit)break;if(l.transition=="unchanged")continue;++rows;append(QStringLiteral("%1 原 %2 → 补丁 %3：%4 / %5（%6 → %7）\n").arg(q(f.file).left(2048)).arg(l.original_line).arg(l.patched_line).arg(q(l.mapping),q(l.transition),q(l.original.state),q(l.patched.state)));}}
    }
    if(rows>=500||text.size()>=limit)text+=QStringLiteral("\n界面摘要已截断，完整行记录见 comparison.json。\n");return text;
}
