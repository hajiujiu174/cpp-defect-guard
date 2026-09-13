#pragma once
#include "codeguard/application.hpp"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/SHA256.h>
#include <fstream>
#include <climits>
#include <set>

namespace codeguard::tu_cache {
inline std::string digest(llvm::StringRef data) { llvm::SHA256 hash;hash.update(data);return llvm::toHex(hash.final(),true); }
inline void add(llvm::SHA256& hash,llvm::StringRef data){hash.update(std::to_string(data.size())+":");hash.update(data);}
struct UnitOutput {
    TranslationUnitResult unit;
    std::map<std::string,Symbol> symbols;
    std::map<std::string,FunctionMetric> metrics;
    std::set<std::string> covered;
    std::vector<GraphEdge> edges;
    std::vector<Issue> issues;
    bool cached=false;
    bool cache_checked=false,cache_error=false;
    std::int64_t validation_ms=0;
};
// Re-run the current compiler's preprocessor before reuse. This redoes include
// search and negative __has_include checks, including system/external headers.
// It avoids trusting an old list of positive dependencies or file timestamps.
struct Probe {
    llvm::SHA256 trace,output;
    std::map<std::string,std::string> dependencies;
    bool reusable=true;
    std::string fingerprint() {
        llvm::SHA256 hash;add(hash,llvm::toHex(trace.final(),true));add(hash,llvm::toHex(output.final(),true));
        for(const auto& [path,value]:dependencies){add(hash,path);add(hash,value);}return llvm::toHex(hash.final(),true);
    }
};
class Trace final : public clang::PPCallbacks {
    Probe& probe;clang::SourceManager& sources;
    void buffer(clang::FileID id){
        bool invalid=false;const auto data=sources.getBufferData(id,&invalid);
        if(invalid){probe.reusable=false;return;}
        const auto name=sources.getBufferName(sources.getLocForStartOfFile(id)).str();const auto hash=digest(data);
        const auto [it,inserted]=probe.dependencies.emplace(name,hash);
        if(!inserted&&it->second!=hash)probe.reusable=false;
        add(probe.trace,name);add(probe.trace,hash);
    }
public:
    Trace(Probe& p,clang::SourceManager& s):probe(p),sources(s){}
    void FileChanged(clang::SourceLocation loc,FileChangeReason reason,clang::SrcMgr::CharacteristicKind kind,clang::FileID) override {
        add(probe.trace,std::to_string(reason));add(probe.trace,std::to_string(kind));
        if(reason==EnterFile)buffer(sources.getFileID(sources.getExpansionLoc(loc)));
    }
    void InclusionDirective(clang::SourceLocation,const clang::Token&,llvm::StringRef name,bool angled,clang::CharSourceRange,
        clang::OptionalFileEntryRef file,llvm::StringRef search,llvm::StringRef relative,const clang::Module*,bool,clang::SrcMgr::CharacteristicKind kind) override {
        add(probe.trace,name);add(probe.trace,angled?"angle":"quote");add(probe.trace,search);add(probe.trace,relative);add(probe.trace,std::to_string(kind));
        add(probe.trace,file?file->getName():llvm::StringRef{});
        // Also hash resolved headers skipped by include guards or pragma once.
        if(file){auto contents=sources.getFileManager().getBufferForFile(*file);if(!contents){probe.reusable=false;return;}
            const auto hash=digest((*contents)->getBuffer());const auto [it,inserted]=probe.dependencies.emplace(file->getName().str(),hash);
            if(!inserted&&it->second!=hash)probe.reusable=false;}
    }
    void HasInclude(clang::SourceLocation,llvm::StringRef name,bool angled,clang::OptionalFileEntryRef file,clang::SrcMgr::CharacteristicKind kind) override {
        add(probe.trace,"has_include");add(probe.trace,name);add(probe.trace,angled?"angle":"quote");add(probe.trace,file?file->getName():llvm::StringRef{});add(probe.trace,std::to_string(kind));
    }
    void HasEmbed(clang::SourceLocation,llvm::StringRef,bool,clang::OptionalFileEntryRef) override {probe.reusable=false;}
    void EmbedDirective(clang::SourceLocation,llvm::StringRef,bool,clang::OptionalFileEntryRef,const clang::LexEmbedParametersResult&) override {probe.reusable=false;}
    void If(clang::SourceLocation,clang::SourceRange,ConditionValueKind value) override {add(probe.trace,"if"+std::to_string(value));}
    void Elif(clang::SourceLocation,clang::SourceRange,ConditionValueKind value,clang::SourceLocation) override {add(probe.trace,"elif"+std::to_string(value));}
    void MacroExpands(const clang::Token& token,const clang::MacroDefinition&,clang::SourceRange,const clang::MacroArgs*) override {
        if(const auto* id=token.getIdentifierInfo()){const auto name=id->getName();if(name=="__DATE__"||name=="__TIME__"||name=="__TIMESTAMP__")probe.reusable=false;}
    }
};
class HashStream final : public llvm::raw_ostream {
    llvm::SHA256& hash;std::uint64_t position=0;
    void write_impl(const char* data,std::size_t size) override {hash.update(llvm::StringRef(data,size));position+=size;}
    std::uint64_t current_pos() const override {return position;}
public:
    explicit HashStream(llvm::SHA256& value):hash(value){}
    ~HashStream() override {flush();}
};
class ProbeAction final : public clang::PreprocessorFrontendAction {
    Probe& probe;
public:
    explicit ProbeAction(Probe& p):probe(p){}
    void ExecuteAction() override {
        auto& compiler=getCompilerInstance();
        // Includes actual target, language, driver-generated system include paths,
        // diagnostic settings and other effective cc1 options.
        compiler.getInvocation().generateCC1CommandLine([&](const llvm::Twine& arg){add(probe.trace,arg.str());});
        auto& pp=compiler.getPreprocessor();pp.addPPCallbacks(std::make_unique<Trace>(probe,compiler.getSourceManager()));
        clang::PreprocessorOutputOptions options;options.ShowCPP=1;options.ShowComments=1;options.ShowMacroComments=1;options.ShowMacros=1;
        HashStream stream(probe.output);clang::DoPrintPreprocessedInput(pp,&stream,options);stream.flush();
    }
};

inline std::string encode(const UnitOutput& part,const std::string& key){
    using llvm::json::Array;using llvm::json::Object;
    // Evidence may end halfway through a UTF-8 code point. Preserve arbitrary
    // bytes exactly instead of feeding invalid UTF-8 to LLVM's JSON strings.
    auto text=[](const std::string& s){return llvm::toHex(llvm::StringRef(s),true);};
    Array symbols,metrics,edges,issues,covered;
    for(const auto& [id,s]:part.symbols)symbols.push_back(Array{text(s.id),text(s.kind),text(s.name),text(s.file),s.line,s.column,s.definition,s.external});
    for(const auto& [id,m]:part.metrics)metrics.push_back(Array{text(m.symbol_id),m.lines,m.parameters,m.complexity});
    for(const auto& e:part.edges)edges.push_back(Array{text(e.kind),text(e.source),text(e.target),text(e.file),e.line,e.column});
    for(const auto& i:part.issues)issues.push_back(Array{text(i.rule_id),text(i.severity),text(i.file),i.line,i.column,text(i.message),text(i.evidence),text(i.suggestion),text(i.symbol_id),text(i.detector)});
    for(const auto& path:part.covered)covered.push_back(text(path));
    Object result{{"version",1},{"key",key},{"unit",Array{text(part.unit.file),text(part.unit.status),text(part.unit.diagnostics),part.unit.indirect_calls,text(part.unit.command_fingerprint)}},
        {"symbols",std::move(symbols)},{"metrics",std::move(metrics)},{"edges",std::move(edges)},{"issues",std::move(issues)},{"covered",std::move(covered)}};
    return llvm::formatv("{0}",llvm::json::Value(std::move(result))).str();
}
inline UnitOutput decode(const std::string& payload,const std::string& key){
    auto parsed=llvm::json::parse(payload);if(!parsed){llvm::consumeError(parsed.takeError());throw std::runtime_error("invalid cache JSON");}
    const auto* object=parsed->getAsObject();if(!object||object->getInteger("version")!=1||object->getString("key")!=key)throw std::runtime_error("invalid cache version or key");
    auto rows=[&](const char* field)->const llvm::json::Array&{const auto* value=object->getArray(field);if(!value)throw std::runtime_error("missing cache array");return *value;};
    auto row=[](const llvm::json::Value& value,std::size_t count)->const llvm::json::Array&{const auto* a=value.getAsArray();if(!a||a->size()!=count)throw std::runtime_error("invalid cache row");return *a;};
    auto string=[](const llvm::json::Value& value){auto s=value.getAsString();if(!s||s->size()%2||!std::all_of(s->begin(),s->end(),[](char c){return llvm::isHexDigit(c);}))throw std::runtime_error("invalid cache text");return llvm::fromHex(*s);};
    auto integer=[](const llvm::json::Value& value){auto n=value.getAsInteger();if(!n||*n<INT_MIN||*n>INT_MAX)throw std::runtime_error("invalid cache integer");return static_cast<int>(*n);};
    auto boolean=[](const llvm::json::Value& value){auto b=value.getAsBoolean();if(!b)throw std::runtime_error("invalid cache boolean");return *b;};
    UnitOutput part;const auto& unit=rows("unit");if(unit.size()!=5)throw std::runtime_error("invalid cache unit");
    part.unit={string(unit[0]),string(unit[1]),string(unit[2]),integer(unit[3]),string(unit[4])};
    if(part.unit.status!="success")throw std::runtime_error("only successful AST results can be reused");
    for(const auto& value:rows("symbols")){const auto& r=row(value,8);Symbol s{string(r[0]),string(r[1]),string(r[2]),string(r[3]),integer(r[4]),integer(r[5]),boolean(r[6]),boolean(r[7])};if(!part.symbols.emplace(s.id,s).second)throw std::runtime_error("duplicate cache symbol");}
    for(const auto& value:rows("metrics")){const auto& r=row(value,4);FunctionMetric m{string(r[0]),integer(r[1]),integer(r[2]),integer(r[3])};if(!part.symbols.contains(m.symbol_id)||!part.metrics.emplace(m.symbol_id,m).second)throw std::runtime_error("invalid cache metric reference");}
    for(const auto& value:rows("edges")){const auto& r=row(value,6);part.edges.push_back({string(r[0]),string(r[1]),string(r[2]),string(r[3]),integer(r[4]),integer(r[5])});}
    for(const auto& value:rows("issues")){const auto& r=row(value,10);part.issues.push_back({string(r[0]),string(r[1]),string(r[2]),integer(r[3]),integer(r[4]),string(r[5]),string(r[6]),string(r[7]),string(r[8]),string(r[9])});}
    for(const auto& value:rows("covered"))part.covered.insert(string(value));
    part.unit.covered_files.assign(part.covered.begin(),part.covered.end());part.cached=true;return part;
}
inline std::optional<UnitOutput> read(const fs::path& directory,const std::string& key){
    const auto path=directory/key/"entry";
    if(!fs::exists(path))return {};
    if(fs::is_symlink(directory/key)||fs::is_symlink(path)||fs::file_size(path)>128*1024*1024)throw std::runtime_error("unsupported cache entry");
    std::ifstream stream(path,std::ios::binary);std::string hash;std::getline(stream,hash);
    std::string payload((std::istreambuf_iterator<char>(stream)),{});
    if(!stream||hash!=digest(payload))throw std::runtime_error("incomplete or corrupted cache entry");
    return decode(payload,key);
}
inline void write(const fs::path& directory,const std::string& key,const UnitOutput& part){
    const auto payload=encode(part,key);if(payload.size()>128*1024*1024)return;
    fs::create_directories(directory);
    // Exclusive ownership per content key. Existing or interrupted entries are
    // never overwritten; invalid entries simply fall back to a fresh AST parse.
    const auto entry=directory/key;if(!fs::create_directory(entry))return;
    std::ofstream stream(entry/"entry",std::ios::binary);stream<<digest(payload)<<'\n'<<payload;stream.close();
    if(!stream)throw std::runtime_error("cannot save AST cache entry");
}
} // namespace codeguard::tu_cache
