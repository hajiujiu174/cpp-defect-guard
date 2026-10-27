#pragma once
#include "codeguard/application.hpp"
#include "codeguard/graph_explorer.hpp"
#include "read_task.hpp"
#include <QWidget>
#include <functional>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableView;
class GraphPathModel;
class GraphCanvas;

class GraphPanel final : public QWidget {
public:
    explicit GraphPanel(QWidget* parent=nullptr);
    ~GraphPanel() override;
    void setSnapshot(std::shared_ptr<const codeguard::ScanResult> snapshot);
    void focusNode(const std::string& id);
    bool busy() const;
    std::function<void(const std::string&,int,int)> navigate;
protected:
    void showEvent(QShowEvent* event) override;
private:
    struct Result {
        std::shared_ptr<const codeguard::GraphIndex> index;
        std::optional<codeguard::GraphSearch> search;
        std::optional<codeguard::GraphSlice> slice;
        std::optional<codeguard::GraphPath> path;
    };
    using Work=std::function<Result(const codeguard::ScanContext&)>;
    void submit(Work work);
    void startPending();
    void buildIndex();
    void search();
    void neighborhood();
    void path();
    void accept(Result result);
    void updateButtons();
    std::string resolve(const QLineEdit* input) const;
    void selectNode(QLineEdit* input,const std::string& id);
    std::string defaultFocus() const;
    void clearView();
    ReadTask<Result> task_;
    std::optional<Work> pending_;
    std::uint64_t version_=0,working_version_=0;
    std::shared_ptr<const codeguard::ScanResult> snapshot_;
    std::shared_ptr<const codeguard::GraphIndex> index_;
    QComboBox *kind_,*direction_;
    QLineEdit *query_,*focus_,*from_,*to_;
    QSpinBox *depth_,*limit_;
    QPushButton *search_,*expand_,*find_path_,*cancel_;
    QLabel *status_,*details_;
    QListWidget* matches_;
    QTableView* steps_;
    GraphPathModel* path_model_;
    GraphCanvas* canvas_;
};
