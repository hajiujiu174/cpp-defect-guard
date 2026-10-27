#include "graph_panel.hpp"
#include <QComboBox>
#include <QAction>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableView>
#include <QAbstractTableModel>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <map>
#include <stdexcept>

namespace cg=codeguard;
namespace {
QString q(const std::string& s){return QString::fromUtf8(s.data(),static_cast<int>(s.size()));}
std::string u(const QString& s){return s.toUtf8().toStdString();}
class EdgeItem;
class NodeItem final : public QGraphicsItem {
public:
    cg::GraphNode node;
    std::vector<EdgeItem*> edges;
    std::function<void()> activated,selected;
    explicit NodeItem(cg::GraphNode value):node(std::move(value)) {
        setFlags(ItemIsMovable|ItemIsSelectable|ItemSendsGeometryChanges);setZValue(1);setCursor(Qt::OpenHandCursor);
        setToolTip(q(node.label)+"\n"+q(node.id)+"\n"+q(node.file)+":"+QString::number(node.line));
        setData(0,"node");setData(1,q(node.id));
    }
    QRectF boundingRect() const override{return {-112,-29,224,58};}
    void paint(QPainter* painter,const QStyleOptionGraphicsItem*,QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing);
        QPen pen(isSelected()?QColor("#1d4ed8"):QColor(node.external?"#94a3b8":"#93b4df"),isSelected()?2.5:1.3);
        if(node.external)pen.setStyle(Qt::DashLine);painter->setPen(pen);
        painter->setBrush(isSelected()?QColor("#dbeafe"):QColor(node.external?"#f1f5f9":"#ffffff"));
        painter->drawRoundedRect(boundingRect().adjusted(2,2,-2,-2),8,8);
        auto font=painter->font();font.setPointSize(10);font.setBold(true);painter->setFont(font);painter->setPen(QColor("#24344b"));
        painter->drawText(QRectF(-100,-20,200,22),Qt::AlignCenter,QFontMetrics(font).elidedText(q(node.label),Qt::ElideMiddle,198));
        font.setPointSize(8);font.setBold(false);painter->setFont(font);painter->setPen(QColor("#64748b"));
        const auto detail=(node.external?QStringLiteral("外部 · "):QString())+QFileInfo(q(node.file)).fileName()+":"+QString::number(node.line);
        painter->drawText(QRectF(-100,3,200,20),Qt::AlignCenter,QFontMetrics(font).elidedText(detail,Qt::ElideMiddle,198));
    }
protected:
    QVariant itemChange(GraphicsItemChange change,const QVariant& value) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override {if(activated)activated();event->accept();}
};
class EdgeItem final : public QGraphicsPathItem {
public:
    NodeItem *source,*target;
    cg::GraphLink link;
    std::function<void()> activated,selected;
    EdgeItem(NodeItem* a,NodeItem* b,cg::GraphLink value):source(a),target(b),link(std::move(value)) {
        setFlags(ItemIsSelectable);setZValue(-1);setData(0,"edge");setData(1,q(link.evidence.source));setData(2,q(link.evidence.target));
        setToolTip(q(link.evidence.file)+":"+QString::number(link.evidence.line)+":"+QString::number(link.evidence.column)+
            QStringLiteral("\n已保存源码位置：%1；双击定位首个位置").arg(link.location_count));
        source->edges.push_back(this);if(source!=target)target->edges.push_back(this);updatePath();
    }
    void updatePath() {
        const auto a=source->pos(),b=target->pos();QPainterPath curve;
        if(source==target){curve.moveTo(a+QPointF(106,0));curve.cubicTo(a+QPointF(180,-110),a+QPointF(-30,-110),a+QPointF(0,-28));}
        else {
            const auto delta=b-a;auto endpoint=[](const QPointF& center,const QPointF& vector){
                const auto scale=std::min(110.0/std::max(std::abs(vector.x()),0.001),28.0/std::max(std::abs(vector.y()),0.001));
                return center+vector*scale;
            };
            const auto length=std::hypot(delta.x(),delta.y());
            if(length<1){curve.moveTo(a);curve.lineTo(a+QPointF(1,0));}
            else {curve.moveTo(endpoint(a,delta));curve.quadTo((a+b)/2+QPointF(-delta.y(),delta.x())*(18.0/length),endpoint(b,-delta));}
        }
        setPath(curve);
    }
    QRectF boundingRect() const override{return QGraphicsPathItem::boundingRect().adjusted(-12,-12,12,12);}
    QPainterPath shape() const override {QPainterPathStroker stroke;stroke.setWidth(14);return stroke.createStroke(path());}
    void paint(QPainter* painter,const QStyleOptionGraphicsItem*,QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing);const QColor color(isSelected()?"#1d4ed8":"#7189aa");
        painter->setPen(QPen(color,isSelected()?3:1.7));painter->setBrush(Qt::NoBrush);painter->drawPath(path());
        const auto end=path().pointAtPercent(1),before=path().pointAtPercent(0.96);const auto delta=end-before;
        const auto angle=std::atan2(delta.y(),delta.x());
        QPolygonF arrow;arrow<<end<<end-QPointF(std::cos(angle-0.45)*12,std::sin(angle-0.45)*12)<<end-QPointF(std::cos(angle+0.45)*12,std::sin(angle+0.45)*12);
        painter->setBrush(color);painter->drawPolygon(arrow);
    }
protected:
    QVariant itemChange(GraphicsItemChange change,const QVariant& value) override {
        if(change==ItemSelectedHasChanged&&value.toBool()&&selected)selected();return QGraphicsPathItem::itemChange(change,value);
    }
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override {if(activated)activated();event->accept();}
};
QVariant NodeItem::itemChange(GraphicsItemChange change,const QVariant& value) {
    if(change==ItemPositionHasChanged)for(auto* edge:edges)edge->updatePath();
    if(change==ItemSelectedHasChanged&&value.toBool()&&selected)selected();return QGraphicsItem::itemChange(change,value);
}
}

class GraphCanvas final : public QGraphicsView {
    bool auto_fit_=true;
public:
    std::function<void(const std::string&,int,int)> navigate;
    std::function<void(const QString&)> detail;
    explicit GraphCanvas(QWidget* parent):QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));setRenderHint(QPainter::Antialiasing);setDragMode(ScrollHandDrag);
        setTransformationAnchor(AnchorUnderMouse);setMinimumHeight(140);setObjectName("graphCanvas");setBackgroundBrush(QColor("#f8fafc"));
    }
    void showSlice(const cg::GraphSlice& slice) {
        scene()->clear();std::map<std::string,NodeItem*> items;std::map<int,int> rows;
        for(const auto& entry:slice.nodes) {
            auto* item=new NodeItem(entry.node);scene()->addItem(item);items[entry.node.id]=item;
            item->setPos(entry.layer*290,rows[entry.layer]++*95);
            item->activated=[this,node=entry.node]{if(navigate&&!node.file.empty())navigate(node.file,node.line,node.column);};
            item->selected=[this,node=entry.node]{if(detail)detail(q(node.label)+"\n"+q(node.id)+"\n"+q(node.file)+":"+QString::number(node.line)+":"+QString::number(node.column));};
        }
        for(const auto& link:slice.links) {
            auto* edge=new EdgeItem(items.at(link.evidence.source),items.at(link.evidence.target),link);scene()->addItem(edge);
            edge->activated=[this,link]{if(navigate&&!link.evidence.file.empty())navigate(link.evidence.file,link.evidence.line,link.evidence.column);};
            edge->selected=[this,link]{if(detail)detail(q(link.evidence.source)+QStringLiteral(" → ")+q(link.evidence.target)+"\n"+
                q(link.evidence.file)+":"+QString::number(link.evidence.line)+QStringLiteral(" · %1 个已保存位置").arg(link.location_count));};
        }
        scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-65,-65,65,65));fit();
    }
    void fit(){auto_fit_=true;resetTransform();if(!scene()->items().isEmpty()){scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-65,-65,65,65));fitInView(scene()->itemsBoundingRect().adjusted(-40,-40,40,40),Qt::KeepAspectRatio);if(transform().m11()>1.3)scale(1.3/transform().m11(),1.3/transform().m11());}}
    void zoom(double factor){auto_fit_=false;const auto next=transform().m11()*factor;if(next>=0.03&&next<=4.0)scale(factor,factor);}
protected:
    void resizeEvent(QResizeEvent* event) override {QGraphicsView::resizeEvent(event);if(auto_fit_)fit();}
    void wheelEvent(QWheelEvent* event) override {zoom(event->angleDelta().y()>0?1.18:1/1.18);event->accept();}
};

class GraphPathModel final : public QAbstractTableModel {
public:
    explicit GraphPathModel(QObject* parent):QAbstractTableModel(parent){}
    cg::GraphPath path;
    void replace(cg::GraphPath value={}){beginResetModel();path=std::move(value);endResetModel();}
    int rowCount(const QModelIndex& parent={}) const override{return parent.isValid()?0:static_cast<int>(path.links.size());}
    int columnCount(const QModelIndex& parent={}) const override{return parent.isValid()?0:3;}
    QVariant headerData(int section,Qt::Orientation orientation,int role=Qt::DisplayRole) const override {
        if(role!=Qt::DisplayRole)return {};if(orientation==Qt::Vertical)return section+1;
        return QStringList{QStringLiteral("路径边"),QStringLiteral("源码位置"),QStringLiteral("位置数")}.value(section);
    }
    QVariant data(const QModelIndex& index,int role=Qt::DisplayRole) const override {
        if(!index.isValid()||index.row()>=rowCount())return {};const auto i=static_cast<std::size_t>(index.row());const auto& link=path.links[i];
        if(role==Qt::UserRole)return q(link.evidence.file);if(role==Qt::UserRole+1)return link.evidence.line;if(role==Qt::UserRole+2)return link.evidence.column;
        if(role==Qt::ToolTipRole)return q(link.evidence.source)+QStringLiteral(" → ")+q(link.evidence.target);
        if(role!=Qt::DisplayRole)return {};
        if(index.column()==0)return q(path.nodes[i].label)+QStringLiteral(" → ")+q(path.nodes[i+1].label);
        if(index.column()==1)return q(link.evidence.file)+":"+QString::number(link.evidence.line)+":"+QString::number(link.evidence.column);
        return QVariant::fromValue(static_cast<qulonglong>(link.location_count));
    }
};

GraphPanel::GraphPanel(QWidget* parent):QWidget(parent),task_(this) {
    setObjectName("graphPanel");auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea(this);scroll->setObjectName("graphScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    auto* content=new QWidget(scroll);scroll->setWidget(content);outer->addWidget(scroll);
    auto* layout=new QVBoxLayout(content);layout->setContentsMargins(4,4,4,4);layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* row=new QHBoxLayout;kind_=new QComboBox(this);kind_->setObjectName("graphKind");kind_->addItem(QStringLiteral("调用图"),"call");kind_->addItem(QStringLiteral("包含图"),"include");
    query_=new QLineEdit(this);query_->setObjectName("graphSearch");query_->setPlaceholderText(QStringLiteral("检索函数名、文件或符号 ID"));
    search_=new QPushButton(QStringLiteral("检索"),this);search_->setObjectName("graphSearchButton");cancel_=new QPushButton(QStringLiteral("取消"),this);cancel_->setObjectName("graphCancel");
    auto* reload=new QPushButton(QStringLiteral("重建"),this);reload->setObjectName("graphReload");connect(reload,&QPushButton::clicked,this,[this]{buildIndex();});
    row->addWidget(kind_);row->addWidget(query_,1);row->addWidget(search_);row->addWidget(reload);row->addWidget(cancel_);layout->addLayout(row);
    auto* body=new QWidget(this);auto* body_layout=new QVBoxLayout(body);body_layout->setContentsMargins(0,0,0,0);
    auto* upper=new QWidget(body);auto* upper_layout=new QVBoxLayout(upper);upper_layout->setContentsMargins(0,0,0,0);
    matches_=new QListWidget(upper);matches_->setObjectName("graphMatches");matches_->setMinimumHeight(32);matches_->setMaximumHeight(80);matches_->hide();upper_layout->addWidget(matches_);
    auto* neighborhood_row=new QHBoxLayout;focus_=new QLineEdit(this);focus_->setObjectName("graphFocus");focus_->setPlaceholderText(QStringLiteral("中心节点：从检索结果选择"));
    direction_=new QComboBox(this);direction_->setObjectName("graphDirection");direction_->addItems({QStringLiteral("出向"),QStringLiteral("入向"),QStringLiteral("双向")});
    direction_->setToolTip(QStringLiteral("出向：调用的函数 / 包含的文件；入向：调用方 / 包含方"));
    depth_=new QSpinBox(this);depth_->setObjectName("graphDepth");depth_->setRange(0,8);depth_->setValue(2);depth_->setPrefix(QStringLiteral("深度 "));
    limit_=new QSpinBox(this);limit_->setObjectName("graphLimit");limit_->setRange(10,200);limit_->setValue(60);limit_->setPrefix(QStringLiteral("上限 "));
    expand_=new QPushButton(QStringLiteral("展开"),this);expand_->setObjectName("graphExpand");
    neighborhood_row->addWidget(focus_,1);neighborhood_row->addWidget(direction_);neighborhood_row->addWidget(depth_);neighborhood_row->addWidget(limit_);neighborhood_row->addWidget(expand_);upper_layout->addLayout(neighborhood_row);
    auto* path_row=new QHBoxLayout;from_=new QLineEdit(this);to_=new QLineEdit(this);from_->setObjectName("graphFrom");to_->setObjectName("graphTo");
    from_->setPlaceholderText(QStringLiteral("路径起点：唯一名称或 ID"));to_->setPlaceholderText(QStringLiteral("路径终点：唯一名称或 ID"));
    find_path_=new QPushButton(QStringLiteral("最短有向路径"),this);find_path_->setObjectName("graphFindPath");path_row->addWidget(from_,1);path_row->addWidget(to_,1);path_row->addWidget(find_path_);upper_layout->addLayout(path_row);
    auto* canvas_panel=new QWidget(body);auto* canvas_layout=new QVBoxLayout(canvas_panel);canvas_layout->setContentsMargins(0,0,0,0);
    canvas_=new GraphCanvas(canvas_panel);canvas_->navigate=[this](const auto& f,int l,int c){if(navigate)navigate(f,l,c);};canvas_->detail=[this](const auto& value){details_->setText(value);};canvas_layout->addWidget(canvas_,1);
    auto* tools=new QHBoxLayout;for(const auto& [label,factor]:std::vector<std::pair<QString,double>>{{QStringLiteral("缩小"),1/1.2},{QStringLiteral("放大"),1.2},{QStringLiteral("适应"),0}}){
        auto* button=new QPushButton(label,this);tools->addWidget(button);connect(button,&QPushButton::clicked,this,[this,factor]{if(factor)canvas_->zoom(factor);else canvas_->fit();});
    }tools->addStretch();canvas_layout->addLayout(tools);body_layout->addWidget(upper);body_layout->addWidget(canvas_panel,1);layout->addWidget(body,1);
    steps_=new QTableView(this);steps_->setObjectName("graphPathSteps");path_model_=new GraphPathModel(this);steps_->setModel(path_model_);
    steps_->setEditTriggers(QAbstractItemView::NoEditTriggers);steps_->setSelectionBehavior(QAbstractItemView::SelectRows);steps_->verticalHeader()->hide();steps_->horizontalHeader()->setStretchLastSection(true);steps_->setMinimumHeight(40);steps_->setMaximumHeight(110);steps_->hide();layout->addWidget(steps_);
    details_=new QLabel(QStringLiteral("拖动节点调整布局，拖动空白平移，滚轮缩放；双击节点或路径边定位源码。"),this);details_->setObjectName("graphDetails");details_->setWordWrap(true);details_->setTextInteractionFlags(Qt::TextSelectableByMouse);details_->setMaximumHeight(74);layout->addWidget(details_);
    status_=new QLabel(QStringLiteral("载入含 Clang 分析的快照后查看关系图。"),this);status_->setObjectName("graphStatus");status_->setWordWrap(true);layout->addWidget(status_);
    connect(kind_,&QComboBox::currentIndexChanged,this,[this]{++version_;task_.cancel();pending_.reset();index_.reset();focus_->clear();from_->clear();to_->clear();clearView();updateButtons();if(snapshot_&&isVisible())buildIndex();});
    connect(search_,&QPushButton::clicked,this,[this]{search();});connect(query_,&QLineEdit::returnPressed,this,[this]{search();});
    connect(expand_,&QPushButton::clicked,this,[this]{neighborhood();});connect(focus_,&QLineEdit::returnPressed,this,[this]{neighborhood();});
    connect(find_path_,&QPushButton::clicked,this,[this]{path();});
    connect(matches_,&QListWidget::itemClicked,this,[this](QListWidgetItem* item){selectNode(focus_,u(item->data(Qt::UserRole).toString()));});
    connect(matches_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem* item){selectNode(focus_,u(item->data(Qt::UserRole).toString()));neighborhood();});
    matches_->setContextMenuPolicy(Qt::ActionsContextMenu);matches_->setToolTip(QStringLiteral("双击展开；右键将选中节点设为路径起点或终点"));
    for(const auto& [name,input]:std::vector<std::pair<QString,QLineEdit*>>{{QStringLiteral("设为路径起点"),from_},{QStringLiteral("设为路径终点"),to_}}){
        auto* action=new QAction(name,matches_);action->setObjectName(input==from_?"graphSetFrom":"graphSetTo");matches_->addAction(action);
        connect(action,&QAction::triggered,this,[this,input]{if(auto* item=matches_->currentItem())selectNode(input,u(item->data(Qt::UserRole).toString()));});
    }
    for(auto* input:{focus_,from_,to_})connect(input,&QLineEdit::textEdited,this,[input]{input->setProperty("graphNodeId",{});});
    connect(steps_,&QTableView::doubleClicked,this,[this](const QModelIndex& index){const auto item=path_model_->index(index.row(),0);if(navigate&&!item.data(Qt::UserRole).toString().isEmpty())navigate(u(item.data(Qt::UserRole).toString()),item.data(Qt::UserRole+1).toInt(),item.data(Qt::UserRole+2).toInt());});
    connect(cancel_,&QPushButton::clicked,this,[this]{pending_.reset();++version_;task_.cancel();status_->setText(QStringLiteral("关系图任务已取消，现有画布保留"));updateButtons();});
    task_.finished=[this](ReadOutcome<Result> outcome){
        if(working_version_==version_){
            if(outcome.result)accept(std::move(*outcome.result));
            else if(!outcome.error.empty())status_->setText(QStringLiteral("关系图失败：")+q(outcome.error));
            else if(outcome.cancelled)status_->setText(QStringLiteral("关系图任务已取消"));
        }
        startPending();updateButtons();
    };
    updateButtons();
}
GraphPanel::~GraphPanel(){task_.finished={};task_.cancel();}
bool GraphPanel::busy() const{return task_.busy()||pending_.has_value();}
void GraphPanel::showEvent(QShowEvent* event){QWidget::showEvent(event);if(snapshot_&&!index_&&!busy())buildIndex();}
void GraphPanel::clearView(){canvas_->scene()->clear();matches_->clear();matches_->hide();path_model_->replace();steps_->hide();details_->clear();}
void GraphPanel::setSnapshot(std::shared_ptr<const cg::ScanResult> snapshot) {
    ++version_;task_.cancel();pending_.reset();snapshot_=std::move(snapshot);index_.reset();clearView();focus_->clear();from_->clear();to_->clear();
    for(auto* input:{focus_,from_,to_})input->setProperty("graphNodeId",{});
    status_->setText(QStringLiteral("快照已更新；图只展示成功解析配置中的已保存关系。"));updateButtons();if(isVisible()&&snapshot_)buildIndex();
}
void GraphPanel::submit(Work work){++version_;task_.cancel();pending_=std::move(work);startPending();updateButtons();}
void GraphPanel::startPending(){if(task_.busy()||!pending_)return;auto work=std::move(*pending_);pending_.reset();working_version_=version_;task_.start(std::move(work));}
void GraphPanel::updateButtons(){const bool ready=bool(index_);search_->setEnabled(ready);expand_->setEnabled(ready);find_path_->setEnabled(ready);cancel_->setEnabled(busy());}
void GraphPanel::buildIndex(){
    if(!snapshot_)return;const auto snapshot=snapshot_;const auto kind=u(kind_->currentData().toString());index_.reset();
    status_->setText(QStringLiteral("正在后台建立关系索引…"));
    submit([snapshot,kind](const cg::ScanContext& context){Result r;r.index=std::make_shared<cg::GraphIndex>(cg::build_graph_index(*snapshot,kind,context));return r;});
}
std::string GraphPanel::defaultFocus() const {
    for(const auto& [id,n]:index_->nodes)if(!n.external&&n.label=="main")return id;
    for(const auto& [id,n]:index_->nodes)if(!n.external&&index_->outgoing.contains(id))return id;
    return index_->nodes.empty()?"":index_->nodes.begin()->first;
}
void GraphPanel::selectNode(QLineEdit* input,const std::string& id) {
    if(!index_||!index_->nodes.contains(id)){input->setProperty("graphNodeId",{});input->setText(q(id));return;}
    input->setProperty("graphNodeId",q(id));input->setText(q(index_->nodes.at(id).label));input->setToolTip(q(id));
}
std::string GraphPanel::resolve(const QLineEdit* input) const {
    const auto chosen=u(input->property("graphNodeId").toString());const auto needle=u(input->text().trimmed());
    if(index_->nodes.contains(chosen)&&index_->nodes.at(chosen).label==needle)return chosen;
    if(index_->nodes.contains(needle))return needle;std::string id;
    for(const auto& [key,node]:index_->nodes)if(node.label==needle){if(!id.empty())throw std::invalid_argument("Ambiguous node name; choose an exact ID from search results");id=key;}
    if(id.empty())throw std::invalid_argument("Node not found; search and select a node first");return id;
}
void GraphPanel::focusNode(const std::string& id){
    kind_->setCurrentIndex(0);selectNode(focus_,id);selectNode(from_,id);
    if(index_){if(index_->nodes.contains(id))neighborhood();else status_->setText(QStringLiteral("此符号不是已保存调用图中的函数节点"));}
    else if(!busy())buildIndex();
}
void GraphPanel::search(){if(!index_)return;auto index=index_;const auto needle=u(query_->text());status_->setText(QStringLiteral("正在检索节点…"));submit([index,needle](const cg::ScanContext& c){Result r;r.search=cg::search_graph_nodes(*index,needle,100,c);return r;});}
void GraphPanel::neighborhood(){
    if(!index_)return;try{const auto id=resolve(focus_);auto index=index_;const auto direction=static_cast<cg::GraphDirection>(direction_->currentIndex());
        const unsigned depth=depth_->value();const auto limit=static_cast<std::size_t>(limit_->value());status_->setText(QStringLiteral("正在后台提取邻域…"));
        submit([index,id,direction,depth,limit](const cg::ScanContext& c){Result r;r.slice=cg::graph_neighborhood(*index,id,direction,depth,limit,600,c);return r;});
    }catch(const std::exception& e){status_->setText(QStringLiteral("请选择节点：")+q(e.what()));}
}
void GraphPanel::path(){
    if(!index_)return;try{const auto from=resolve(from_),to=resolve(to_);auto index=index_;status_->setText(QStringLiteral("正在后台查找最短有向路径…"));
        submit([index,from,to](const cg::ScanContext& c){Result r;r.path=cg::shortest_graph_path(*index,from,to,c);return r;});
    }catch(const std::exception& e){status_->setText(QStringLiteral("请选择路径端点：")+q(e.what()));}
}
void GraphPanel::accept(Result result){
    if(result.index){index_=std::move(result.index);matches_->clear();std::string id;
        try{id=focus_->text().isEmpty()?defaultFocus():resolve(focus_);}catch(const std::invalid_argument&){id=defaultFocus();}
        if(id.empty()){status_->setText(QStringLiteral("此快照没有该类型的图节点"));return;}
        selectNode(focus_,id);selectNode(from_,id);neighborhood();return;
    }
    const auto coverage=QStringLiteral(" · 分析 %1 · 仅表示已保存的静态关系，非运行时可达性证明").arg(q(index_->analysis_status));
    if(result.search){matches_->clear();matches_->show();for(const auto& node:result.search->nodes){auto* item=new QListWidgetItem(q(node.label)+" ["+q(node.file)+":"+QString::number(node.line)+"]",matches_);item->setData(Qt::UserRole,q(node.id));item->setToolTip(q(node.id));}
        status_->setText(QStringLiteral("匹配 %1 个节点，显示前 %2 个；双击检索结果展开").arg(result.search->total).arg(result.search->nodes.size())+coverage);}
    if(result.slice){path_model_->replace();steps_->hide();canvas_->showSlice(*result.slice);status_->setText(QStringLiteral("邻域深度 %1 · 显示 %2/%3 个节点、%4/%5 条关系%6%7")
        .arg(result.slice->depth).arg(result.slice->nodes.size()).arg(index_->nodes.size()).arg(result.slice->links.size()).arg(index_->links.size())
        .arg(result.slice->node_limit_reached?QStringLiteral(" · 已达节点上限"):QString()).arg(result.slice->edge_limit_reached?QStringLiteral(" · 已达关系上限"):QString())+coverage);}
    if(result.path){auto& path=*result.path;path_model_->replace();steps_->hide();
        if(!path.found){canvas_->scene()->clear();status_->setText(QStringLiteral("已保存图中未找到有向路径；不能据此证明运行时不可达")+coverage);return;}
        // The complete path remains in the table. Rendering alone is capped.
        cg::GraphSlice slice;const auto shown=std::min<std::size_t>(path.nodes.size(),200);
        for(std::size_t i=0;i<shown;++i)slice.nodes.push_back({path.nodes[i],static_cast<int>(i%10)});
        for(std::size_t i=0;i+1<shown;++i)slice.links.push_back(path.links[i]);canvas_->showSlice(slice);
        status_->setText(QStringLiteral("最短有向路径 %1 条边 · 已访问 %2 个节点%3").arg(path.links.size()).arg(path.visited)
            .arg(shown<path.nodes.size()?QStringLiteral(" · 画布仅显示前 200 个节点；路径表保留全部边"):QString())+coverage);
        path_model_->replace(std::move(path));
        steps_->setVisible(path_model_->rowCount()>0);
    }
}
