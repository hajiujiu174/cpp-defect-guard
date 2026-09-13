#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include "codeguard/report.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;namespace fs=std::filesystem;
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void write(const fs::path& path,const std::string& contents){std::ofstream out(path,std::ios::binary);out<<contents;require(bool(out),"fixture write");}
struct Fixture {
    fs::path dir=fs::current_path()/("cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=dir/"source",external=dir/"external",database=dir/"scan.sqlite3",cache=dir/"scan.sqlite3.tu-cache";
    cg::ScanOptions options;
    Fixture(){
        fs::create_directories(root);fs::create_directory(external);
        write(root/"api.h","#pragma once\ninline int common(int n){if(n) return n;return 0;}\n");
        write(external/"external.h","#define OFFSET 1\n");
        write(root/"a.cpp","#include \"api.h\"\n#include <external.h>\n#if __has_include(<optional.h>)\n#define INDEX 3\n#else\n#define INDEX 1\n#endif\nint alpha(){int a[2]={};return a[INDEX]+OFFSET+common(1);}\n");
        write(root/"b.cpp","#include \"api.h\"\nint beta(){return common(2);}\n");
        write(root/"c.cpp","#ifndef FLAG\n#define FLAG 1\n#endif\nint gamma(){int a[2]={};return a[FLAG];}\n");
        commands();options.compile_commands=cg::utf8_path(dir);options.threads=1;
    }
    ~Fixture(){std::error_code error;fs::remove_all(dir,error);}
    void commands(const std::string& flag="1"){
        std::ofstream out(dir/"compile_commands.json");out<<'[';
        int n=0;for(const auto* file:{"a.cpp","b.cpp","c.cpp"}){
            if(n++)out<<',';out<<"{\"directory\":\""<<cg::utf8_path(root)<<"\",\"file\":\""<<file<<"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-I"<<cg::utf8_path(external)<<"\",\"-c\",\""<<file<<"\"";
            if(std::string(file)=="c.cpp")out<<",\"-DFLAG="<<flag<<"\"";out<<"]}";
        }out<<']';require(bool(out),"commands write");
    }
    cg::ScanResult scan(bool cached=true){auto copy=options;copy.use_cache=cached;return cg::import_project(root,database,copy);}
};
void equivalent(cg::ScanResult a,cg::ScanResult b){
    auto normalize=[](cg::ScanResult& scan){scan.id=1;scan.scanned_at="0";scan.added=scan.changed=scan.unchanged=scan.removed=0;scan.analysis.elapsed_ms=0;scan.analysis.workers=0;auto config=cg::decode_config(scan.analysis.configuration);config.threads=0;scan.analysis.configuration=cg::encode_config(config);};
    normalize(a);normalize(b);
    require(cg::report_json(a)==cg::report_json(b),"cached and full snapshot evidence differ");
    for(const auto* table:{"symbols","functions","edges","issues","suppressed_issues","rule_diagnostics"}){
        const auto query="SELECT * FROM "+std::string(table);require(cg::execute_query(a,query).rows==cg::execute_query(b,query).rows,"cached/full query mismatch: "+query);
    }
}
void stats(const cg::ScanResult& s,std::size_t hits,std::size_t misses){require(s.id>0&&s.analysis.status=="complete"&&s.analysis.cache&&s.analysis.cache->hits==hits&&s.analysis.cache->misses==misses,"unexpected cache stats: "+(s.analysis.cache?std::to_string(s.analysis.cache->hits)+" hits / "+std::to_string(s.analysis.cache->misses)+" misses":"absent"));}
void run(const std::string& mode,const std::string& cli){
    Fixture f;
    if(mode=="stability"){
        int probes=0;f.options.context.progress=[&](const cg::ScanProgress& p){if(p.phase=="cache_validating"&&++probes==2)write(f.external/"external.h","#define OFFSET 9\n");};
        bool failed=false;try{f.scan();}catch(const std::exception& e){failed=std::string(e.what()).find("dependency changed")!=std::string::npos;}
        require(failed&&!fs::exists(f.database),"changing external dependency was committed");return;
    }
    if(mode=="cli"){
        auto call=[&](const std::string& cache){return cg::run_process({{cli,"scan",cg::utf8_path(f.root),"--database",cg::utf8_path(f.database),"--compile-commands",cg::utf8_path(f.dir),"--threads","2","--cache",cache},cg::utf8_path(f.dir),std::chrono::seconds(30)});};
        require(call("on").exit_code==0,"cold CLI failed");const auto warm=call("on");require(warm.exit_code==0&&(warm.stdout_text.find("cache_hits=3\n")!=std::string::npos||warm.stdout_text.find("cache_hits=3\r\n")!=std::string::npos),"new CLI process did not reuse persisted cache: "+warm.stdout_text+warm.stderr_text);
        const auto full=call("off");require(full.exit_code==0&&full.stdout_text.find("cache_hits=")==std::string::npos,"--cache off did not force full parsing");return;
    }
    auto cold=f.scan();stats(cold,0,3);auto warm=f.scan();stats(warm,3,0);equivalent(warm,f.scan(false));
    if(mode=="reuse"){
        f.options.threads=4;const auto parallel=f.scan();stats(parallel,3,0);equivalent(cold,parallel);
        write(f.root/"b.cpp","#include \"api.h\"\nint beta(){if(common(2))return 1;return 0;}\n");
        const auto changed=f.scan();stats(changed,2,1);equivalent(changed,f.scan(false));
    }else if(mode=="dependencies"){
        const auto stamp=fs::last_write_time(f.external/"external.h");write(f.external/"external.h","#define OFFSET 2\n");fs::last_write_time(f.external/"external.h",stamp);
        auto changed=f.scan();stats(changed,2,1);equivalent(changed,f.scan(false));
        write(f.root/"api.h","#pragma once\ninline int common(int n){if(n>1) return n;return 0;}\n");changed=f.scan();stats(changed,1,2);equivalent(changed,f.scan(false));
        write(f.external/"optional.h","// new optional include\n");changed=f.scan();stats(changed,2,1);require(changed.analysis.issues.size()==1,"new __has_include condition didn't change findings");equivalent(changed,f.scan(false));
        fs::remove(f.external/"optional.h");changed=f.scan();stats(changed,3,0);require(changed.analysis.issues.empty(),"deleted optional header kept stale finding");equivalent(changed,f.scan(false));
        write(f.external/"optional.h","// new optional include\n");
        f.commands("3");changed=f.scan();stats(changed,2,1);require(changed.analysis.issues.size()==2,"new compilation flag didn't change findings");equivalent(changed,f.scan(false));
        // A header that shadows a previous include location must also invalidate.
        write(f.root/"external.h","#define OFFSET 7\n");write(f.root/"a.cpp","#include \"external.h\"\nint alpha(){return OFFSET;}\n");changed=f.scan();stats(changed,0,3);equivalent(changed,f.scan(false));
    }else if(mode=="policy"){
        f.commands("3");const auto issue=f.scan();require(issue.analysis.issues.size()==1,"policy fixture issue missing");
        f.options.rule_severities["CG002"]="info";f.options.suppressions={cg::suppress_issue(issue,issue.analysis.issues.front(),"reviewed")};
        auto changed=f.scan();stats(changed,3,0);require(changed.analysis.issues.empty()&&changed.analysis.suppressed_issues.size()==1&&changed.analysis.suppressed_issues.front().severity=="info","cached rule policy not applied exactly once");equivalent(changed,f.scan(false));
        f.options.disabled_rules={"CG002"};changed=f.scan();stats(changed,0,3);equivalent(changed,f.scan(false));
    }else if(mode=="failures"){
        for(const auto& directory:fs::directory_iterator(f.cache))write(directory.path()/"entry","corrupted cache\n");
        auto changed=f.scan();stats(changed,0,3);require(changed.analysis.cache->errors==3,"corrupt entries were not reported");equivalent(changed,f.scan(false));
        write(f.root/"a.cpp","int broken( {\n");changed=f.scan();require(changed.analysis.status=="partial"&&changed.analysis.units.front().status=="parse_failed","parse failure hidden by cache");equivalent(changed,f.scan(false));
        write(f.root/"a.cpp","const char* time_string=__TIME__;\n");changed=f.scan();require(changed.analysis.cache->bypassed==1,"time-dependent source was cached");
        const auto before=changed.id;f.options.context.control=std::make_shared<cg::ScanControl>();
        f.options.context.progress=[&](const cg::ScanProgress& p){if(p.phase=="analyzing"&&p.completed)f.options.context.control->request_cancel();};
        bool cancelled=false;try{f.scan();}catch(const cg::ScanCancelled&){cancelled=true;}
        cg::SqliteDatabase db(f.database,true);require(cancelled&&db.latest(cg::utf8_path(f.root)).id==before,"cancelled cached scan published a snapshot");
    }else throw std::runtime_error("unknown cache test");
}
int main(int argc,char** argv){try{require(argc==3,"mode and CLI path required");run(argv[1],argv[2]);std::cout<<"CACHE_OK "<<argv[1]<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
