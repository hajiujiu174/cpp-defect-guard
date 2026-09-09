#include "source_editor.hpp"
#include <QPainter>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QFontDatabase>

namespace {
class Gutter final : public QWidget {
    SourceEditor* editor_;
public:
    explicit Gutter(SourceEditor* editor) : QWidget(editor), editor_(editor) {}
    QSize sizeHint() const override { return {editor_->gutterWidth(), 0}; }
protected:
    void paintEvent(QPaintEvent* event) override { editor_->paintGutter(event); }
};
class CppHighlighter final : public QSyntaxHighlighter {
public:
    explicit CppHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}
protected:
    void highlightBlock(const QString& text) override {
        static const QRegularExpression keywords(QStringLiteral(
            "\\b(auto|bool|break|case|catch|char|class|const|constexpr|continue|default|delete|do|double|else|enum|explicit|extern|false|float|for|if|inline|int|long|namespace|new|nullptr|private|protected|public|return|short|signed|sizeof|static|struct|switch|template|this|throw|true|try|typedef|typename|union|unsigned|using|virtual|void|volatile|while)\\b"));
        static const QRegularExpression numbers(QStringLiteral("\\b(0[xX][0-9a-fA-F]+|[0-9]+(?:\\.[0-9]+)?)\\b"));
        static const QRegularExpression directive(QStringLiteral("^\\s*#\\s*[A-Za-z_]+"));
        for (const auto& [expression, color] : {std::pair{keywords, QColor("#6d28d9")},
                std::pair{numbers, QColor("#b45309")}, std::pair{directive, QColor("#0f766e")}}) {
            auto matches = expression.globalMatch(text);
            while (matches.hasNext()) { const auto match = matches.next(); setFormat(match.capturedStart(), match.capturedLength(), color); }
        }
        // Small lexical pass: quoted strings do not accidentally open comments.
        bool comment = previousBlockState() == 1;
        int i = 0;
        while (i < text.size()) {
            const int start = i;
            if (comment || text.mid(i, 2) == "/*") {
                const int end = text.indexOf("*/", comment ? i : i + 2);
                i = end < 0 ? text.size() : end + 2;
                setFormat(start, i - start, QColor("#7b8b99")); comment = end < 0;
            } else if (text.mid(i, 2) == "//") {
                setFormat(i, text.size() - i, QColor("#7b8b99")); break;
            } else if (text[i] == '"' || text[i] == '\'') {
                const auto quote = text[i++];
                while (i < text.size()) {
                    if (text[i] == '\\') { i += 2; continue; }
                    if (text[i++] == quote) break;
                }
                setFormat(start, qMin(i, static_cast<int>(text.size())) - start, QColor("#b45309"));
            } else ++i;
        }
        setCurrentBlockState(comment ? 1 : 0);
    }
};
}
SourceEditor::SourceEditor(QWidget* parent) : QPlainTextEdit(parent), gutter_(new Gutter(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (QFontDatabase::families().contains("Consolas")) font.setFamily("Consolas");
    font.setPointSize(11); setFont(font); gutter_->setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    new CppHighlighter(document());
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this]() { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy) gutter_->scroll(0, dy); else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        if (rect.contains(viewport()->rect())) setViewportMargins(gutterWidth(), 0, 0, 0);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(QColor("#eaf3ff"));
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor(); selection.cursor.clearSelection();
        setExtraSelections({selection}); gutter_->update();
    });
    setViewportMargins(gutterWidth(), 0, 0, 0);
}
int SourceEditor::gutterWidth() const {
    return 20 + fontMetrics().horizontalAdvance('9') * QString::number(qMax(1, blockCount())).size();
}
void SourceEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const auto rect = contentsRect(); gutter_->setGeometry(rect.left(), rect.top(), gutterWidth(), rect.height());
}
void SourceEditor::paintGutter(QPaintEvent* event) {
    QPainter painter(gutter_); painter.fillRect(event->rect(), QColor("#f5f7fb"));
    auto block = firstVisibleBlock(); int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top()) {
            painter.setPen(number == textCursor().blockNumber() ? QColor("#2563eb") : QColor("#94a3b8"));
            painter.drawText(0, top, gutter_->width() - 8, fontMetrics().height(), Qt::AlignRight, QString::number(number + 1));
        }
        top += height; block = block.next(); ++number;
    }
}
void SourceEditor::goToLine(int line) {
    auto block = document()->findBlockByNumber(qBound(0, line - 1, blockCount() - 1));
    setTextCursor(QTextCursor(block)); centerCursor(); setFocus();
}
