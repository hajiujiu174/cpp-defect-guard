#include "read_task.hpp"
#include "history_read.hpp"
#include "query_model.hpp"
#include "report_dialog.hpp"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <sqlite3.h>
#include <iostream>
#include <atomic>
#include <thread>

namespace cg=codeguard;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Pulse {
    QElapsedTimer clock; QTimer timer; QLineEdit input;
    qint64 last=0,max_gap=0; int ticks=0;
    Pulse(){clock.start();timer.setInterval(5);QObject::connect(&timer,&QTimer::timeout,[this]{const auto now=clock.elapsed();max_gap=std::max(max_gap,now-last);last=now;input.setText(QString::number(++ticks));});timer.start();}
    void verify(){max_gap=std::max(max_gap,clock.elapsed()-last);require(ticks>0&&input.text()==QString::number(ticks),"GUI input did not update");require(max_gap<500,"GUI heartbeat blocked for 500 ms");}
};
void until(const std::function<bool()>& done){
    QEventLoop loop;QTimer tick,deadline;tick.setInterval(5);deadline.setSingleShot(true);
    QObject::connect(&tick,&QTimer::timeout,&loop,[&]{if(done())loop.quit();});
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    tick.start();deadline.start(30000);if(!done())loop.exec();require(done(),"read acceptance timed out");
}
struct Fixture {
    QTemporaryDir temporary;cg::fs::path database;cg::ScanResult snapshot;
    Fixture(){
        require(temporary.isValid(),"temporary directory unavailable");const auto dir=cg::from_utf8(temporary.path().toStdString());database=dir/"scan.sqlite3";
        snapshot.root=cg::utf8_path(dir/"missing-source");snapshot.scanned_at="1789300000000";
        for(int i=0;i<12000;++i)snapshot.files.push_back({"file"+std::to_string(i)+".cpp","C++","h",10,0,2});
        cg::SqliteDatabase db(database);db.save(snapshot);
    }
};
struct Lock {
    sqlite3* db=nullptr;
    explicit Lock(const cg::fs::path& path){require(sqlite3_open(cg::utf8_path(path).c_str(),&db)==SQLITE_OK,"lock open failed");require(sqlite3_exec(db,"BEGIN EXCLUSIVE",nullptr,nullptr,nullptr)==SQLITE_OK,"lock failed");}
    ~Lock(){sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(db);}
};
void query(){
    auto snapshot=std::make_shared<cg::ScanResult>();snapshot->id=9;
    for(int i=0;i<250000;++i)snapshot->files.push_back({"file"+std::to_string(i)+".cpp","C++","hash",10,0,static_cast<unsigned>(i)});
    ReadTask<cg::QueryResult> task;QueryModel model;QTableView view;view.setModel(&model);view.show();Pulse pulse;
    bool finished=false;ReadOutcome<cg::QueryResult> output;
    task.finished=[&](auto result){output=std::move(result);finished=true;};
    const auto started=pulse.clock.elapsed();
    require(task.start([snapshot](const cg::ScanContext& c){return cg::execute_query(*snapshot,"SELECT file, lines FROM files ORDER BY lines DESC",c);}),"query start failed");
    until([&]{return finished;});require(output.result&&output.result->rows.size()==250000,"large query truncated or failed");
    model.replace(std::move(*output.result));require(model.rowCount()==250000&&model.data(model.index(0,1)).toString()=="249999"&&model.data(model.index(249999,1)).toString()=="0","virtual table mismatch");
    const auto query_ms=pulse.clock.elapsed()-started;finished=false;bool accepted=false;qint64 cancel_ms=0;
    task.start([snapshot](const cg::ScanContext& c){return cg::execute_query(*snapshot,"SELECT * FROM files ORDER BY file DESC",c);});
    QTimer::singleShot(10,[&]{cancel_ms=pulse.clock.elapsed();accepted=task.cancel();});
    until([&]{return finished;});require(accepted&&output.cancelled&&!output.result,"cancelled query delivered a result");
    require(pulse.clock.elapsed()-cancel_ms<2000,"query cancellation took 2 seconds");
    // A result that is ready but has not been polled must still be discardable.
    finished=false;std::atomic<bool> returned=false;
    task.start([&](const cg::ScanContext&){returned=true;return cg::QueryResult{};});
    while(!returned.load())std::this_thread::yield();require(task.cancel(),"ready cancellation rejected");
    until([&]{return finished;});require(output.cancelled&&!output.result,"ready result escaped cancellation");
    pulse.verify();std::cout<<"QUERY_READ_OK rows=250000 elapsed_ms="<<query_ms<<" ticks="<<pulse.ticks<<" max_ui_gap_ms="<<pulse.max_gap<<'\n';
}
void history(){
    Fixture f;ReadTask<HistoryRead> task;ReadOutcome<HistoryRead> output;bool finished=false;Pulse pulse;
    task.finished=[&](auto result){output=std::move(result);finished=true;};
    {
        Lock lock(f.database);qint64 cancelled=0;
        task.start([&](const cg::ScanContext& c){return read_project(cg::utf8_path(f.database),f.snapshot.root,c);});
        QTimer::singleShot(80,[&]{cancelled=pulse.clock.elapsed();task.cancel();});
        until([&]{return finished;});require(output.cancelled&&!output.result,"locked history did not cancel");
        require(pulse.clock.elapsed()-cancelled<1000,"locked read ignored cancellation for a second");
    }
    finished=false;task.start([&](const cg::ScanContext& c){return read_project(cg::utf8_path(f.database),{},c);});
    until([&]{return finished;});require(output.result&&output.result->snapshot->id==f.snapshot.id&&output.result->snapshot->files.size()==12000,"history restart lost snapshot");
    require(output.result->configuration.has_value(),"archived missing source prevented configuration load");
    cg::SqliteDatabase db(f.database,true);require(db.scans(f.snapshot.root).size()==1,"read cancellation changed saved history");
    pulse.verify();std::cout<<"HISTORY_READ_OK files=12000 ticks="<<pulse.ticks<<" max_ui_gap_ms="<<pulse.max_gap<<'\n';
}
void report(){
    Fixture f;Pulse pulse;
    {
        Lock lock(f.database);ReportDialog dialog(f.snapshot.root,f.database);dialog.show();
        QTimer::singleShot(80,&dialog,[&]{dialog.reject();});
        until([&]{return !dialog.isVisible();});
    }
    ReportDialog dialog(f.snapshot.root,f.database);dialog.show();
    auto* generate=dialog.findChild<QPushButton*>("reportGenerate");until([&]{return generate->isEnabled();});
    auto* scans=dialog.findChild<QComboBox*>("reportScan");require(scans->count()==1,"report history missing");
    const auto output=f.database.parent_path()/"cancelled-report";
    dialog.findChild<QLineEdit*>("reportOutput")->setText(QString::fromStdString(cg::utf8_path(output)));
    generate->click();dialog.findChild<QPushButton*>("reportCancel")->click();
    until([&]{return generate->isEnabled();});require(!cg::fs::exists(output),"cancel created a partial report");
    require(scans->count()==1,"cancel lost report selection");
    generate->click();until([&]{return !dialog.property("reportExported").toString().isEmpty();});
    require(cg::fs::file_size(output/"report.html")>1000,"report retry failed");dialog.reject();
    for(const auto* phase:{"report_json","report_html","report_ready"}){
        cg::ScanContext context;context.control=std::make_shared<cg::ScanControl>();bool cancelled=false;
        context.progress=[&](const cg::ScanProgress& p){if(p.phase==phase)context.control->request_cancel();};
        const auto target=f.database.parent_path()/phase;
        try{cg::export_report(f.snapshot,target,nullptr,context);}catch(const cg::ScanCancelled&){cancelled=true;}
        require(cancelled&&!cg::fs::exists(target),"report phase cancellation was not atomic");
    }
    pulse.verify();std::cout<<"REPORT_READ_OK ticks="<<pulse.ticks<<" max_ui_gap_ms="<<pulse.max_gap<<'\n';
}
int main(int argc,char** argv){QApplication app(argc,argv);try{require(argc==2,"case required");const std::string mode=argv[1];if(mode=="query")query();else if(mode=="history")history();else if(mode=="report")report();else throw std::runtime_error("unknown case");return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
