#pragma once
#include <QPlainTextEdit>

class SourceEditor final : public QPlainTextEdit {
public:
    explicit SourceEditor(QWidget* parent = nullptr);
    int gutterWidth() const;
    void paintGutter(QPaintEvent* event);
    void goToLine(int line);
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    QWidget* gutter_;
};
