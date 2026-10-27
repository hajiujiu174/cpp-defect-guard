#include "graph_panel.hpp"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QElapsedTimer>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableView>
#include <QTest>
#include <iostream>

namespace cg=codeguard;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void until(const std::function<bool()>& done){QElapsedTimer clock;clock.start();while(!done()&&clock.elapsed()<30000){QApplication::processEvents();QTest::qWait(5);}check(done(),"graph GUI timeout");}
template<class T> T* child(GraphPanel& panel,const char* name){auto* found=panel.findChild<T*>(name);check(found,"graph widget missing");return found;}
std::shared_ptr<cg::ScanResult> fixture(int count=5){
    auto s=std::make_shared<cg::ScanResult>();s->id=1;s->analysis.status="partial";
    for(int i=0;i<count;++i){const auto id=std::to_string(i);s->analysis.symbols.push_back({id,"function",i==0?"main":i==1||i==2?"target":"fn"+id,"main.cpp",i+1,i+2,true,false});
        if(i+1<count)s->analysis.edges.push_back({"call",id,std::to_string(i+1),"main.cpp",i+10,i+3});}
    s->analysis.edges.push_back({"include","main.cpp","api.h","main.cpp",1,1});s->files.push_back({"main.cpp","C++","hash",10,0,5});return s;
}
int main(int argc,char** argv){QApplication app(argc,argv);try{
    check(argc==2,"mode required");const std::string mode=argv[1];GraphPanel panel;panel.resize(1100,920);panel.setSnapshot(fixture());panel.show();
    auto* canvas=child<QGraphicsView>(panel,"graphCanvas");auto* status=child<QLabel>(panel,"graphStatus");auto* steps=child<QTableView>(panel,"graphPathSteps");
    until([&]{return !panel.busy();});check(!canvas->scene()->items().isEmpty(),"lazy graph did not render");
    std::string file;int line=0,column=0;panel.navigate=[&](const auto& f,int l,int c){file=f;line=l;column=c;};
    if(mode=="workflow") {
        check(status->text().contains("partial")&&status->text().contains(QStringLiteral("深度")),"coverage/depth not visible");
        child<QLineEdit>(panel,"graphSearch")->setText("target");child<QPushButton>(panel,"graphSearchButton")->click();until([&]{return !panel.busy();});
        auto* matches=child<QListWidget>(panel,"graphMatches");check(matches->count()==2,"overloaded search collapsed identities");
        child<QLineEdit>(panel,"graphFrom")->setText("target");child<QLineEdit>(panel,"graphTo")->setText("4");child<QPushButton>(panel,"graphFindPath")->click();
        check(status->text().contains("Ambiguous"),"ambiguous names silently resolved");
        matches->setCurrentRow(0);matches->findChild<QAction*>("graphSetFrom")->trigger();
        check(child<QLineEdit>(panel,"graphFrom")->text()=="target","selected overload should display its name");
        child<QPushButton>(panel,"graphFindPath")->click();until([&]{return !panel.busy();});
        check(steps->model()->rowCount()==3,"selected overload identity was lost behind its display name");
        child<QLineEdit>(panel,"graphFrom")->setText("0");child<QPushButton>(panel,"graphFindPath")->click();until([&]{return !panel.busy();});
        check(steps->model()->rowCount()==4&&steps->model()->data(steps->model()->index(0,1)).toString()=="main.cpp:10:3","path model lost evidence");
        const auto step=steps->model()->index(1,0);steps->doubleClicked(step);check(file=="main.cpp"&&line==11&&column==4,"path edge navigation wrong");
        QGraphicsItem* node=nullptr;QGraphicsPathItem* edge=nullptr;
        for(auto* item:canvas->scene()->items()){if(item->data(0)=="node"&&item->data(1)=="0")node=item;if(item->data(0)=="edge"&&item->data(1)=="0")edge=dynamic_cast<QGraphicsPathItem*>(item);}
        check(node&&edge,"canvas node or directed edge missing");const auto old=edge->path();node->setPos(node->pos()+QPointF(0,60));check(edge->path()!=old,"dragged node left disconnected edge geometry");
        node->setSelected(true);check(child<QLabel>(panel,"graphDetails")->text().contains("main.cpp:1:2"),"node selection lost definition column");
        const auto point=canvas->mapFromScene(node->scenePos());QTest::mouseDClick(canvas->viewport(),Qt::LeftButton,Qt::NoModifier,point);QApplication::processEvents();
        check(file=="main.cpp"&&line==1&&column==2,"canvas double click did not navigate");
        panel.grab().save("graph-workflow.png");panel.resize(640,700);QApplication::processEvents();panel.grab().save("graph-workflow-compact.png");
        panel.resize(380,380);QApplication::processEvents();
        auto* scroll=child<QScrollArea>(panel,"graphScroll");check(scroll->verticalScrollBar()->maximum()>0,"compact graph should preserve content through scrolling");
        check(canvas->parentWidget()->rect().contains(canvas->geometry()),"compact layout clipped canvas behind its toolbar");
        for(auto* button:canvas->parentWidget()->findChildren<QPushButton*>())check(!button->geometry().intersects(canvas->geometry()),"graph toolbar overlaps canvas");
        panel.resize(1100,920);QApplication::processEvents();
        child<QLineEdit>(panel,"graphFrom")->setText("4");child<QLineEdit>(panel,"graphTo")->setText("0");child<QPushButton>(panel,"graphFindPath")->click();until([&]{return !panel.busy();});
        check(steps->model()->rowCount()==0&&status->text().contains(QStringLiteral("未找到")),"missing path kept a stale successful path");
        child<QComboBox>(panel,"graphKind")->setCurrentIndex(1);until([&]{return !panel.busy();});
        child<QLineEdit>(panel,"graphFrom")->setText("main.cpp");child<QLineEdit>(panel,"graphTo")->setText("api.h");child<QPushButton>(panel,"graphFindPath")->click();until([&]{return !panel.busy();});
        check(steps->model()->rowCount()==1,"include directed path failed");
    } else if(mode=="scale") {
        panel.setSnapshot(fixture(30001));QElapsedTimer clock;clock.start();qint64 last=0,max_gap=0;int ticks=0;QTimer pulse;pulse.setInterval(5);
        QObject::connect(&pulse,&QTimer::timeout,[&]{const auto now=clock.elapsed();max_gap=std::max(max_gap,now-last);last=now;++ticks;});pulse.start();
        until([&]{return !panel.busy();});child<QLineEdit>(panel,"graphFrom")->setText("0");child<QLineEdit>(panel,"graphTo")->setText("30000");child<QPushButton>(panel,"graphFindPath")->click();until([&]{return !panel.busy();});
        check(steps->model()->rowCount()==30000&&steps->model()->data(steps->model()->index(29999,1)).toString()=="main.cpp:30009:30002","long path lost tail evidence");
        check(status->text().contains(QStringLiteral("前 200")),"canvas truncation not disclosed");
        std::size_t nodes=0;for(auto* item:canvas->scene()->items())if(item->data(0)=="node")++nodes;check(nodes==200,"long path exceeded drawing bound");
        QApplication::processEvents();max_gap=std::max(max_gap,clock.elapsed()-last);check(ticks>0&&max_gap<500,"graph operation blocked GUI for 500ms");
        std::cout<<"GRAPH_GUI_SCALE rows=30000 max_ui_gap_ms="<<max_gap<<" ticks="<<ticks<<'\n';
        child<QLineEdit>(panel,"graphFrom")->setText("0");child<QPushButton>(panel,"graphFindPath")->click();child<QPushButton>(panel,"graphCancel")->click();until([&]{return !panel.busy();});
        check(status->text().contains(QStringLiteral("已取消")),"cancelled path was later delivered");
    } else if(mode=="stale") {
        panel.setSnapshot(fixture(30001));auto replacement=fixture(2);replacement->id=2;replacement->analysis.symbols[0].name="replacement";panel.setSnapshot(replacement);
        until([&]{return !panel.busy();});check(!status->text().contains(QStringLiteral("失败")),"replacement graph failed");
        std::size_t count=0;for(auto* item:canvas->scene()->items())if(item->data(0)=="node")++count;check(count==2,"stale index overwrote replacement snapshot");
        panel.setSnapshot(fixture(30001));child<QPushButton>(panel,"graphCancel")->click();until([&]{return !panel.busy();});
        child<QPushButton>(panel,"graphReload")->click();until([&]{return !panel.busy();});check(!canvas->scene()->items().isEmpty(),"cancelled index could not be rebuilt");
        panel.setSnapshot(fixture(30001));child<QComboBox>(panel,"graphKind")->setCurrentIndex(1);until([&]{return !panel.busy();});
        count=0;for(auto* item:canvas->scene()->items())if(item->data(0)=="node")++count;check(count==2,"stale call index overwrote include graph");
        auto unavailable=fixture();unavailable->analysis.status="not_requested";panel.setSnapshot(unavailable);until([&]{return !panel.busy();});
        check(canvas->scene()->items().isEmpty()&&status->text().contains(QStringLiteral("失败")),"inventory-only snapshot pretended to have graph analysis");
    }else throw std::runtime_error("unknown mode");
    std::cout<<"GRAPH_GUI_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
