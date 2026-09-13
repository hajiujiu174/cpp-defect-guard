#pragma once
#include "codeguard/query.hpp"
#include <QAbstractTableModel>
#include <algorithm>
#include <climits>

// Qt requests only visible cells; publishing rows doesn't allocate one widget
// item per cell or measure every string in the result set.
class QueryModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;
    void replace(codeguard::QueryResult result = {}) {
        beginResetModel(); result_ = std::move(result); endResetModel();
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(std::min<std::size_t>(result_.rows.size(), INT_MAX));
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(result_.columns.size());
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount() ||
            (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};
        return QString::fromStdString(codeguard::query_value_text(result_.rows[index.row()][index.column()]));
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override {
        if (role != Qt::DisplayRole) return {};
        if (orientation == Qt::Vertical) return section + 1;
        return section >= 0 && section < columnCount() ? QVariant(QString::fromStdString(result_.columns[section])) : QVariant{};
    }
private:
    codeguard::QueryResult result_;
};
