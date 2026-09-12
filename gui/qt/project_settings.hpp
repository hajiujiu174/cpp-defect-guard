#pragma once
#include "codeguard/configuration.hpp"
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QPlainTextEdit>
#include <QTableWidget>

class ProjectSettings final : public QDialog {
public:
    ProjectSettings(const std::filesystem::path& root, const codeguard::ProjectConfig& config, QWidget* parent = nullptr);
    codeguard::ProjectConfig configuration() const;
private:
    std::filesystem::path root_;
    codeguard::ProjectConfig original_;
    QCheckBox *analysis_, *discover_;
    QLineEdit *commands_, *output_, *c_, *cxx_, *target_, *cmake_, *ctest_, *git_;
    QComboBox *type_, *generator_;
    QSpinBox *threads_, *jobs_, *timeout_;
    QPlainTextEdit *definitions_, *includes_, *excludes_;
    QTableWidget* choices_;
    std::vector<std::pair<std::string,QCheckBox*>> rules_;
    std::map<std::string,std::string> selections_;
    std::string choices_path_;
    void loadCommands();
};
