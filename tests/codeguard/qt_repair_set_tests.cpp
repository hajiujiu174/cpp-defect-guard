#include "repair_set_dialog.hpp"
#include <QApplication>
#include <QDir>
#include <QSpinBox>
#include <QScrollBar>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <fstream>
#include <iostream>

namespace cg=codeguard;
void check(bool value,const std::string& text){if(!value)throw std::runtime_error(text);}
std::string read(const cg::fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
void write(const cg::fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary);out<<text;}
void until(const std::function<bool()>& done){QElapsedTimer elapsed;elapsed.start();while(!done()&&elapsed.elapsed()<90000){QApplication::processEvents();QTest::qWait(5);}check(done(),"repair set UI timed out");}
template<class T>T* child(RepairSetDialog& dialog,const char* name){auto* found=dialog.findChild<T*>(name);check(found,std::string("missing set control: ")+name);return found;}
int main(int argc,char** argv){QApplication app(argc,argv);try{
    check(argc==2,"mode required");const std::string mode=argv[1];QTemporaryDir temp(QDir::tempPath()+"/cgsq-XXXXXX");check(temp.isValid(),"temporary directory unavailable");const auto base=cg::from_utf8(temp.path().toStdString()),root=base/"source",db=base/"scan.sqlite3";cg::fs::create_directory(root);
    const std::map<std::string,std::string> originals{{"f.cpp","int f(int n){if(n=0)return 1;return 0;}\n"},{"g.cpp","int g(int n){if(n=1)return 1;return 0;}\n"},{"main.cpp","int f(int);int g(int);int main(){return f(0)==1&&f(1)==0&&g(1)==1&&g(0)==0?0:1;}\n"}};
    for(const auto& [name,text]:originals)write(root/name,text);write(root/"CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(SetUI LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp f.cpp g.cpp)\nadd_test(NAME behavior COMMAND demo)\n");
    check(cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(base/"configure"),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)}).status=="passed","set UI configure failed");
    cg::ScanOptions scan;scan.compile_commands=cg::utf8_path(base/"configure");auto snapshot=std::make_shared<cg::ScanResult>(cg::import_project(root,db,scan));check(snapshot->analysis.issues.size()==2,"set UI fixture issue count wrong");
    cg::BuildOptions options;options.output_directory=base/"sets";options.cxx_compiler=(mode=="coverage"||mode=="pertest"||mode=="compare")?CODEGUARD_TEST_GNU_CXX:CODEGUARD_TEST_CXX;options.gcov=CODEGUARD_TEST_GCOV;options.timeout=std::chrono::seconds(30);options.jobs=2;
    RepairSetDialog dialog(snapshot,snapshot->analysis.issues,db,options);dialog.show();auto* table=child<QTableWidget>(dialog,"setCandidates");auto* status=child<QLabel>(dialog,"setStatus");until([&]{return !dialog.busy()&&table->rowCount()==4;});
    check(child<QPlainTextEdit>(dialog,"setDiff")->isReadOnly()&&!child<QPushButton>(dialog,"setValidate")->isEnabled(),"set UI auto-selected or allowed editing");
    auto choose=[&](const char* file,const char* recipe,Qt::CheckState state){for(int n=0;n<table->rowCount();++n){auto* item=table->item(n,0);if(item->data(Qt::UserRole).toString()==recipe&&item->data(Qt::UserRole+1).toString()==file){item->setCheckState(state);return;}}throw std::runtime_error("set candidate row not found");};
    if(mode=="cancel"){
        child<QPushButton>(dialog,"setGenerate")->click();child<QPushButton>(dialog,"setCancel")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("取消")),"set generation cancellation lost");
        child<QPushButton>(dialog,"setGenerate")->click();until([&]{return !dialog.busy()&&table->rowCount()==4;});choose("f.cpp","condition.compare",Qt::Checked);choose("g.cpp","condition.compare",Qt::Checked);
        child<QPushButton>(dialog,"setValidate")->click();child<QPushButton>(dialog,"setCancel")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("取消"))&&child<QPlainTextEdit>(dialog,"setResults")->toPlainText().contains("g.cpp"),"cancelled set UI lost targets");
        child<QPushButton>(dialog,"setValidate")->click();child<QPushButton>(dialog,"setClose")->click();until([&]{return !dialog.busy()&&!dialog.isVisible();});
        int records=0;for(const auto& e:cg::fs::recursive_directory_iterator(base/"sets"))if(e.path().filename()=="validation.json")++records;check(records==2,"safe close lost set records");
    }else if(mode=="compare"){
        choose("f.cpp","condition.compare",Qt::Checked);choose("g.cpp","condition.compare",Qt::Checked);child<QPushButton>(dialog,"setCompare")->click();until([&]{return !dialog.busy();});
        const auto result=child<QPlainTextEdit>(dialog,"setResults")->toPlainText();check(status->text().contains(QStringLiteral("对照完成"))&&result.contains("ready_for_review")&&result.contains("f.cpp")&&result.contains("g.cpp"),"set GUI comparison failed: "+result.toStdString());
        dialog.resize(760,600);dialog.findChild<QScrollArea*>()->ensureWidgetVisible(child<QPushButton>(dialog,"setCompare"));QApplication::processEvents();dialog.grab().save("repair-set-comparison.png");
        child<QPushButton>(dialog,"setCompare")->click();child<QPushButton>(dialog,"setClose")->click();until([&]{return !dialog.busy()&&!dialog.isVisible();});int count=0;for(const auto& e:cg::fs::recursive_directory_iterator(base/"sets"))if(e.path().filename()=="comparison.json")++count;check(count==2,"close lost comparison records");
    }else{
        choose("f.cpp","condition.compare",Qt::Checked);choose("f.cpp","condition.intent",Qt::Checked);check(status->text().contains(QStringLiteral("组合被拒绝"))&&!child<QPushButton>(dialog,"setExport")->isEnabled(),"conflicting alternatives were exportable");
        choose("f.cpp","condition.intent",Qt::Unchecked);choose("g.cpp","condition.compare",Qt::Checked);check(child<QPushButton>(dialog,"setExport")->isEnabled()&&child<QPlainTextEdit>(dialog,"setDiff")->toPlainText().contains("g.cpp"),"combined preview missing second file");
        child<QPushButton>(dialog,"setExport")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("已导出组合")),"set export controls failed");
        if(mode=="coverage"||mode=="pertest")child<QCheckBox>(dialog,"setCoverage")->click();if(mode=="pertest"){child<QCheckBox>(dialog,"setPerTest")->click();child<QSpinBox>(dialog,"setTestLimit")->setValue(2);}
        child<QPushButton>(dialog,"setValidate")->click();until([&]{return !dialog.busy();});const auto result=child<QPlainTextEdit>(dialog,"setResults")->toPlainText();check(status->text().contains(QStringLiteral("通过本次门槛"))&&result.contains("f.cpp")&&result.contains("g.cpp")&&result.contains(QStringLiteral("正确性：未评估")),"set validation UI failed: "+status->text().toStdString()+result.toStdString());
        if(mode=="coverage")check(result.count("recorded")==2&&result.contains("collected"),"set UI did not show coverage for both edits");
        if(mode=="pertest")check(result.contains(QStringLiteral("逐测试组覆盖：collected"))&&result.contains(QStringLiteral("请求：behavior")),"set UI omitted replay evidence");
        dialog.grab().save(mode=="pertest"?"repair-set-test-coverage.png":mode=="coverage"?"repair-set-coverage.png":"repair-set-verified.png");dialog.resize(760,600);QApplication::processEvents();dialog.findChild<QScrollArea*>()->ensureWidgetVisible(child<QPlainTextEdit>(dialog,"setResults"));QApplication::processEvents();if(mode=="pertest"){dialog.findChild<QScrollArea*>()->verticalScrollBar()->setValue(dialog.findChild<QScrollArea*>()->verticalScrollBar()->maximum());QApplication::processEvents();}dialog.grab().save(mode=="pertest"?"repair-set-test-coverage-compact.png":mode=="coverage"?"repair-set-coverage-compact.png":"repair-set-compact.png");
        child<QPushButton>(dialog,"setGenerate")->click();until([&]{return !dialog.busy();});check(!child<QPushButton>(dialog,"setValidate")->isEnabled()&&child<QPlainTextEdit>(dialog,"setResults")->isHidden(),"set regeneration reused old evidence");
    }
    for(const auto& [name,text]:originals)check(read(root/name)==text,"set UI modified original source");std::cout<<"REPAIR_SET_GUI_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
