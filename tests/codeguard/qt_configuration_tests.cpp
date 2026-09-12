#include "project_settings.hpp"
#include "codeguard/application.hpp"
#include <QApplication>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTimer>
#include <fstream>
#include <iostream>

namespace cg=codeguard;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){
    QApplication app(argc,argv);
    const auto root=std::filesystem::current_path()/("qt-config-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    int result=0;
    try{
        const auto path=cg::utf8_path(root);
        {std::ofstream out(root/"main.cpp");out<<"int main(){return 0;}\n";}
        {std::ofstream out(root/"compile_commands.json");out<<"[{\"directory\":\""<<path<<"\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-c\",\"main.cpp\",\"-DONE\"]},"
            <<"{\"directory\":\""<<path<<"\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-c\",\"main.cpp\",\"-DTWO\"]}]";}
        cg::ProjectConfig config;config.compile_commands=cg::utf8_path(root/"compile_commands.json");
        const auto commands=cg::inspect_compile_commands(config.compile_commands);
        ProjectSettings dialog(root,config);dialog.show();app.processEvents();
        dialog.findChild<QPushButton*>("loadCommands")->click();auto* table=dialog.findChild<QTableWidget*>("commandChoices");
        check(table->rowCount()==1,"ambiguous source presented once");auto* combo=static_cast<QComboBox*>(table->cellWidget(0,1));
        check(combo->currentIndex()==0&&dialog.configuration().command_choices.empty(),"no automatic variant selection");
        combo->setCurrentIndex(combo->findData(QString::fromStdString(commands[1].fingerprint)));
        dialog.findChild<QLineEdit*>("configC")->setText("clang");dialog.findChild<QLineEdit*>("configCxx")->setText("clang++");
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        check(dialog.result()==QDialog::Accepted,"save button accepts valid config");
        const auto selected=dialog.configuration();check(selected.command_choices.at(commands[1].file)==commands[1].fingerprint,"UI preserves exact command identity");
        check(selected.build.c_compiler=="clang"&&selected.build.cxx_compiler=="clang++","compiler controls captured");
        auto stale=selected;stale.command_choices.clear();stale.command_choices[cg::utf8_path(root/"removed.cpp")]=std::string(64,'a');
        ProjectSettings stale_dialog(root,stale);stale_dialog.findChild<QPushButton*>("loadCommands")->click();
        auto* stale_table=stale_dialog.findChild<QTableWidget*>("commandChoices");check(stale_table->rowCount()==2,"missing file choice remains editable");
        for(int row=0;row<stale_table->rowCount();++row)static_cast<QComboBox*>(stale_table->cellWidget(row,1))->setCurrentIndex(0);
        check(stale_dialog.configuration().command_choices.empty(),"clear missing or stale choice");
        std::cout<<"QT_CONFIG_CHOICES_OK\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    std::error_code error;std::filesystem::remove_all(root,error);return result;
}
