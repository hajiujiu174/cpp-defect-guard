#include "repair_dialog.hpp"
#include <QApplication>
#include <QDir>
#include <QSpinBox>
#include <QScrollBar>
#include <QComboBox>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QScrollArea>
#include <QTest>
#include <fstream>
#include <iostream>

namespace cg=codeguard;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void until(const std::function<bool()>& done){QElapsedTimer clock;clock.start();while(!done()&&clock.elapsed()<60000){QApplication::processEvents();QTest::qWait(5);}check(done(),"repair UI timeout");}
template<class T>T* child(RepairDialog& dialog,const char* name){auto* found=dialog.findChild<T*>(name);check(found,"repair control missing");return found;}
std::string read(const cg::fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
int main(int argc,char** argv){QApplication app(argc,argv);try{
    check(argc==2,"mode required");const std::string mode=argv[1];QTemporaryDir temp(QDir::tempPath()+"/cgt-XXXXXX");check(temp.isValid(),"temporary directory unavailable");const auto base=cg::from_utf8(temp.path().toStdString()),root=base/"source",db=base/"scan.sqlite3",configure=base/"configure";cg::fs::create_directory(root);
    const std::string original=mode=="precedence"?"int f(int n){int error=0;if((error=n<0)||n==0)return error;return 0;}\nint main(){return f(-7)==-7?0:1;}\n":"int f(int n){if(n=0)return 1;return 0;}\nint main(){return f(0)==1&&f(1)==0?0:1;}\n";
    {std::ofstream out(root/"main.cpp",std::ios::binary);out<<original;}{std::ofstream out(root/"CMakeLists.txt");out<<"cmake_minimum_required(VERSION 3.24)\nproject(RepairUI LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp)\nadd_test(NAME behavior COMMAND demo)\n";}
    check(cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(configure),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)}).status=="passed","UI fixture configure failed");
    cg::ScanOptions scan;scan.compile_commands=cg::utf8_path(configure);auto snapshot=std::make_shared<cg::ScanResult>(cg::import_project(root,db,scan));check(snapshot->analysis.issues.size()==1,"UI fixture issue mismatch");
    cg::BuildOptions options;options.output_directory=base/"repairs";options.cxx_compiler=CODEGUARD_TEST_CXX;options.timeout=std::chrono::seconds(20);
#ifdef CODEGUARD_TEST_GNU_CXX
    if(mode=="coverage"||mode=="pertest"||mode=="compare")options.cxx_compiler=CODEGUARD_TEST_GNU_CXX;
#endif
    RepairDialog dialog(snapshot,snapshot->analysis.issues[0],db,options);dialog.show();auto* choices=child<QComboBox>(dialog,"repairCandidates");auto* status=child<QLabel>(dialog,"repairStatus");
    until([&]{return !dialog.busy()&&choices->count()==(mode=="precedence"?3:2);});check(child<QPlainTextEdit>(dialog,"repairDiff")->isReadOnly(),"patch preview is editable");
    if(mode=="precedence"){
        choices->setCurrentIndex(choices->findData("condition.assignment_result"));check(choices->currentText().contains(QStringLiteral("保存返回值"))&&child<QLabel>(dialog,"repairRisk")->text().contains(QStringLiteral("错误处理")),"new precedence recipe is mislabeled");child<QPushButton>(dialog,"repairValidate")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("门槛通过")),"precedence GUI validation failed");dialog.grab().save("repair-precedence.png");
        choices->setCurrentIndex(choices->findData("condition.intent"));child<QPushButton>(dialog,"repairValidate")->click();until([&]{return !dialog.busy();});check(status->text().contains("tests_failed"),"explicit boolean assignment silently repaired return code");
    }else if(mode=="compare"){
#ifdef CODEGUARD_TEST_GNU_CXX
        child<QCheckBox>(dialog,"repairCoverage")->click();child<QLineEdit>(dialog,"repairGcov")->setText(CODEGUARD_TEST_GCOV);choices->setCurrentIndex(choices->findData("condition.compare"));
        auto* rounds=child<QSpinBox>(dialog,"repairRounds");check(rounds->minimum()==2&&rounds->maximum()==5,"round control bounds wrong");child<QPushButton>(dialog,"repairCompare")->click();check(!rounds->isEnabled(),"rounds editable during comparison");until([&]{return !dialog.busy();});
        const auto text=child<QPlainTextEdit>(dialog,"repairResults")->toPlainText();check(status->text().contains(QStringLiteral("对照完成"))&&text.contains("ready_for_review")&&text.contains(QStringLiteral("正确性：未评估")),"single GUI comparison failed");
        dialog.resize(760,600);dialog.findChild<QScrollArea*>()->ensureWidgetVisible(child<QPushButton>(dialog,"repairCompare"));QApplication::processEvents();dialog.grab().save("repair-comparison.png");
        child<QPushButton>(dialog,"repairCompare")->click();child<QPushButton>(dialog,"repairCancel")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("已取消"))&&child<QPlainTextEdit>(dialog,"repairResults")->toPlainText().contains("not_run"),"comparison cancellation lost ledger");
#endif
    }else if(mode=="coverage"||mode=="pertest"){
#ifdef CODEGUARD_TEST_GNU_CXX
        auto* coverage=child<QCheckBox>(dialog,"repairCoverage");auto* gcov=child<QLineEdit>(dialog,"repairGcov");
        check(!coverage->isChecked()&&!gcov->isEnabled(),"coverage default changed");coverage->click();check(gcov->isEnabled(),"coverage tool control stayed disabled");gcov->setText(CODEGUARD_TEST_GCOV);if(mode=="pertest"){check(child<QCheckBox>(dialog,"repairPerTest")->isEnabled(),"per-test control stayed disabled");child<QCheckBox>(dialog,"repairPerTest")->click();child<QSpinBox>(dialog,"repairTestLimit")->setValue(2);}
        choices->setCurrentIndex(choices->findData("condition.compare"));child<QPushButton>(dialog,"repairValidate")->click();until([&]{return !dialog.busy();});
        check(status->text().contains(QStringLiteral("门槛通过"))&&child<QPlainTextEdit>(dialog,"repairResults")->toPlainText().contains(QStringLiteral("覆盖率采集：collected")),"GUI covered validation failed");
        if(mode=="pertest")check(child<QPlainTextEdit>(dialog,"repairResults")->toPlainText().contains(QStringLiteral("逐测试组覆盖：collected")),"single UI omitted per-test evidence");
        child<QPushButton>(dialog,"repairEvaluate")->click();until([&]{return !dialog.busy();});const auto* table=child<QTableWidget>(dialog,"repairBatchTable");
        check(table->rowCount()==2&&table->item(0,5)->text().contains('/')&&table->item(0,6)->text()==QStringLiteral("已记录执行"),"batch UI omitted measured coverage");
        check(status->text().contains(QStringLiteral("覆盖率已采集")),"batch UI calls collected coverage unmeasured");
        dialog.grab().save(mode=="pertest"?"repair-test-coverage.png":"repair-coverage.png");dialog.resize(760,560);QApplication::processEvents();auto* scroll=dialog.findChild<QScrollArea*>();scroll->ensureWidgetVisible(child<QTableWidget>(dialog,"repairBatchTable"));QApplication::processEvents();if(mode=="pertest"){scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());QApplication::processEvents();}dialog.grab().save(mode=="pertest"?"repair-test-coverage-compact.png":"repair-coverage-compact.png");
        gcov->setText(temp.path()+"/missing-gcov.exe");child<QPushButton>(dialog,"repairValidate")->click();until([&]{return !dialog.busy();});check(status->text().contains("coverage_failed"),"GUI hid failed coverage collection");
#else
        throw std::runtime_error("coverage UI tools unavailable");
#endif
    }else if(mode=="workflow"){
        choices->setCurrentIndex(choices->findData("condition.compare"));check(child<QPlainTextEdit>(dialog,"repairDiff")->toPlainText().contains("n==0"),"comparison preview missing");
        child<QPushButton>(dialog,"repairExport")->click();until([&]{return !dialog.busy();});check(status->text().startsWith(QStringLiteral("已导出：")),"native export controls failed");
        child<QPushButton>(dialog,"repairValidate")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("门槛通过")),"native validation did not pass");
        const auto text=child<QPlainTextEdit>(dialog,"repairResults")->toPlainText();check(text.contains(QStringLiteral("修复正确性：未评估"))&&text.contains(QStringLiteral("内容一致")),"validation UI misreported correctness or original source");
        dialog.grab().save("repair-dialog-verified.png");dialog.resize(760,560);QApplication::processEvents();dialog.grab().save("repair-dialog-compact.png");
        choices->setCurrentIndex(choices->findData("condition.intent"));child<QPushButton>(dialog,"repairValidate")->click();until([&]{return !dialog.busy();});
        check(status->text().contains("tests_failed")&&child<QPlainTextEdit>(dialog,"repairResults")->toPlainText().contains(QStringLiteral("测试：失败")),"UI hid a failing behavioral test after warning disappearance");
    }else if(mode=="batch"){
        child<QPushButton>(dialog,"repairEvaluate")->click();until([&]{return !dialog.busy();});
        auto* table=child<QTableWidget>(dialog,"repairBatchTable");check(table->rowCount()==2,"batch table lost candidates");
        check(table->item(0,1)->text()=="condition.compare"&&table->item(0,2)->text()=="gates_passed"&&table->item(1,2)->text()=="tests_failed","GUI batch ranking hid failure or misordered candidates");
        check(status->text().contains(QStringLiteral("1 个通过门槛"))&&status->text().contains(QStringLiteral("覆盖率未测量")),"batch UI conflated gates with coverage");
        auto* scroll=dialog.findChild<QScrollArea*>();check(scroll,"batch scroll area missing");scroll->ensureWidgetVisible(table);QApplication::processEvents();
        QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,table->visualItemRect(table->item(1,1)).center());
        check(choices->currentData()=="condition.intent"&&child<QPlainTextEdit>(dialog,"repairResults")->toPlainText().contains(QStringLiteral("测试：失败")),"batch row did not select its own preview and result");
        choices->setCurrentIndex(choices->findData("condition.compare"));check(child<QPlainTextEdit>(dialog,"repairResults")->toPlainText().contains(QStringLiteral("测试：通过"))&&table->currentRow()==0,"switching candidates showed stale validation or table selection");
        dialog.grab().save("repair-batch-verified.png");dialog.resize(760,560);QApplication::processEvents();scroll->ensureWidgetVisible(table);QApplication::processEvents();dialog.grab().save("repair-batch-compact.png");
        child<QPushButton>(dialog,"repairGenerate")->click();until([&]{return !dialog.busy();});check(table->rowCount()==0&&table->isHidden()&&child<QPlainTextEdit>(dialog,"repairResults")->isHidden(),"regeneration reused stale batch evidence");
    }else if(mode=="batch_cancel"){
        child<QPushButton>(dialog,"repairEvaluate")->click();child<QPushButton>(dialog,"repairCancel")->click();until([&]{return !dialog.busy();});
        check(status->text().contains("cancelled")&&child<QTableWidget>(dialog,"repairBatchTable")->rowCount()==2,"cancelled batch lost its ledger");
        child<QPushButton>(dialog,"repairEvaluate")->click();child<QPushButton>(dialog,"repairClose")->click();until([&]{return !dialog.busy()&&!dialog.isVisible();});
        int records=0;for(const auto& e:cg::fs::recursive_directory_iterator(base/"repairs"))if(e.path().filename()=="batch.json")++records;
        check(records==2,"safe batch close discarded terminal records");
    }else if(mode=="cancel"){
        child<QPushButton>(dialog,"repairGenerate")->click();child<QPushButton>(dialog,"repairCancel")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("取消")),"cancelled proposal was delivered");
        child<QPushButton>(dialog,"repairGenerate")->click();until([&]{return !dialog.busy()&&choices->count()==2;});
        child<QPushButton>(dialog,"repairValidate")->click();child<QPushButton>(dialog,"repairCancel")->click();until([&]{return !dialog.busy();});check(status->text().contains(QStringLiteral("取消")),"cancelled validation overwritten by success");
        child<QPushButton>(dialog,"repairGenerate")->click();child<QPushButton>(dialog,"repairClose")->click();until([&]{return !dialog.busy()&&!dialog.isVisible();});
    }else throw std::runtime_error("unknown repair UI mode");
    check(read(root/"main.cpp")==original,"repair dialog changed original source");std::cout<<"REPAIR_GUI_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
