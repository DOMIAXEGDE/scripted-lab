// scripted.cpp — CLI REPL using shared core (now with :insr and :delr)
// g++ -std=c++23 -O2 scripted.cpp -o scripted.exe
#include "scripted_core.hpp"
#include "scripted_exec.hpp"   // <— ADD THIS
#include <iostream>

using namespace scripted;
using std::string;

struct Editor {
    Paths P;
    Config cfg;
    Workspace ws;
    std::optional<long long> current;
    bool dirty=false;

    void loadConfig(){ cfg = ::scripted::loadConfig(P); }  // note the qualification
    void saveCfg(){ saveConfig(P, cfg); }
    bool ensureCurrent(){ if(!current){ std::cout<<"No current context. Use :open <ctx>\n"; return false;} return true; }

    void help(){
        std::cout <<
R"(Commands:
  :help                          Show this help
  :open <ctx>                    Open/create context (e.g., x00001)
  :switch <ctx>                  Switch current context
  :preload                       Load all banks in files/
  :ls                            List loaded contexts
  :show                          Print current buffer (header + addresses)
  :ins <addr> <value...>         Insert/replace in register 1
  :insr <reg> <addr> <value...>  Insert/replace into a specific register
  :del <addr>                    Delete from register 1
  :delr <reg> <addr>             Delete from a specific register
  :w                             Write current buffer to files/<ctx>.txt
  :r <path>                      Read/merge a raw model snippet from a file
  :resolve                       Write files/out/<ctx>.resolved.txt
  :export                        Write files/out/<ctx>.json
  :set prefix <char>
  :set base <n>
  :set widths bank=5 addr=4 reg=2
  :run_code						 Run Java, C, C++, Python
  :q                             Quit (prompts if dirty)
)" << std::endl;
    }

    void listCtx(){
        if (ws.banks.empty()) { std::cout<<"(no contexts)\n"; return; }
        for (auto& [id,b] : ws.banks){
            std::cout<<cfg.prefix<<toBaseN(id,cfg.base,cfg.widthBank)<<"  ("<<b.title<<")"
                     <<(current && *current==id? " [current]":"")<<"\n";
        }
    }

    void show(){
        if (!ensureCurrent()) return;
        std::cout<<writeBankText(ws.banks[*current], cfg);
    }

    void write(){
        if (!ensureCurrent()) return;
        string err;
        if (!saveContextFile(cfg, contextFileName(cfg, *current), ws.banks[*current], err))
            std::cout<<"Write failed: "<<err<<"\n";
        else { dirty=false; std::cout<<"Saved "<<contextFileName(cfg,*current).string()<<"\n"; }
    }

    void insert(const string& addrTok, const string& value){
        if (!ensureCurrent()) return;
        long long addr;
        if (!parseIntBase(addrTok, cfg.base, addr)) { std::cout<<"Bad address\n"; return; }
        ws.banks[*current].regs[1][addr] = value; dirty=true;
    }

    void insertR(const string& regTok, const string& addrTok, const string& value){
        if (!ensureCurrent()) return;
        long long reg=1, addr=0;
        if (!parseIntBase(regTok, cfg.base, reg))  { std::cout<<"Bad register\n"; return; }
        if (!parseIntBase(addrTok, cfg.base, addr)){ std::cout<<"Bad address\n";  return; }
        ws.banks[*current].regs[reg][addr] = value; dirty=true;
    }

    void del(const string& addrTok){
        if (!ensureCurrent()) return;
        long long addr; if (!parseIntBase(addrTok, cfg.base, addr)) { std::cout<<"Bad address\n"; return; }
        auto& m = ws.banks[*current].regs[1];
        size_t n = m.erase(addr);
        std::cout<<(n? "Deleted.\n":"No such address.\n");
        if (n) dirty=true;
    }

    void delR(const string& regTok, const string& addrTok){
        if (!ensureCurrent()) return;
        long long reg=1, addr=0;
        if (!parseIntBase(regTok, cfg.base, reg))  { std::cout<<"Bad register\n"; return; }
        if (!parseIntBase(addrTok, cfg.base, addr)){ std::cout<<"Bad address\n";  return; }
        auto& regs = ws.banks[*current].regs;
        auto itR = regs.find(reg);
        if (itR==regs.end()){ std::cout<<"No such register.\n"; return; }
        size_t n = itR->second.erase(addr);
        std::cout<<(n? "Deleted.\n":"No such address.\n");
        if (n) dirty=true;
        if (itR->second.empty()) regs.erase(itR); // tidy up empty register
    }

    void readMerge(const string& path){
        if (!ensureCurrent()) return;
        std::ifstream in(path, std::ios::binary);
        if (!in){ std::cout<<"Cannot open "<<path<<"\n"; return; }
        string text( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
        Bank tmp;
        auto pr = parseBankText(text, cfg, tmp);
        if (!pr.ok){ std::cout<<"Parse failed: "<<pr.err<<"\n"; return; }
        for (auto& [rid, addrs] : tmp.regs)
            for (auto& [aid, val] : addrs)
                ws.banks[*current].regs[rid][aid] = val;
        if (ws.banks[*current].title.empty()) ws.banks[*current].title = tmp.title;
        dirty=true; std::cout<<"Merged.\n";
    }

    void resolveOut(){
        if (!ensureCurrent()) return;
        auto txt = resolveBankToText(cfg, ws, *current);
        auto outp = outResolvedName(cfg, *current);
        std::ofstream out(outp, std::ios::binary); out<<txt;
        std::cout<<"Wrote "<<outp<<"\n";
    }

    void exportJson(){
        if (!ensureCurrent()) return;
        auto js = exportBankToJSON(cfg, ws, *current);
        auto outp = outJsonName(cfg, *current);
        std::ofstream out(outp, std::ios::binary); out<<js;
        std::cout<<"Wrote "<<outp<<"\n";
    }

    void repl(){
        P.ensure();
        loadConfig();
        std::cout<<"scripted CLI — shared core\nType :help for commands.\n\n";
		std::cout << "scripted CLI — " << scripted::platformName() << (scripted::isWSL() ? " (WSL)" : "") << "\n";
        string line;
        while (true){
            std::cout<<">> ";
            if (!std::getline(std::cin, line)) break;
            string s = trim(line);
            if (s.empty()) continue;

            if (s==":help"){ help(); continue; }
            if (s==":ls"){ listCtx(); continue; }
            if (s==":show"){ show(); continue; }
            if (s==":w"){ write(); continue; }
            if (s==":preload"){ preloadAll(cfg, ws); std::cout<<"Preloaded "<<ws.banks.size()<<" banks.\n"; continue; }
            if (s==":resolve"){ resolveOut(); continue; }
            if (s==":export"){ exportJson(); continue; }
            if (s==":q"){
                if (dirty){
                    std::cout<<"Unsaved changes. Type :w to save or :q again to quit.\n>> ";
                    string l2; if (!std::getline(std::cin,l2)) break;
                    if (trim(l2)==":q") break; else { s = trim(l2); }
                } else break;
            }

            // tokenized commands
            std::istringstream is(s); std::vector<string> tok;
            for (string t; is>>t;) tok.push_back(t);
            if (tok.empty()) continue;

            if (tok[0]==":open" && tok.size()>=2){
                string status; if (openCtx(cfg, ws, tok[1], status)){
                    string token = (tok[1][0]==cfg.prefix)? tok[1].substr(1): tok[1];
                    long long id; parseIntBase(token, cfg.base, id);
                    current = id;
                }
                std::cout<<status<<"\n"; continue;
            }
			// ... inside your REPL loop:
			if (tok[0] == ":run_code" && tok.size() >= 3) {
				if (!ensureCurrent()) { std::cout << "Open a context first\n"; continue; }

				long long r=0,a=0;
				if (!parseIntBase(tok[1], cfg.base, r) || !parseIntBase(tok[2], cfg.base, a)) {
					std::cout << "Bad reg/addr\n"; continue;
				}

				auto itR = ws.banks[*current].regs.find(r);
				if (itR == ws.banks[*current].regs.end()) { std::cout << "No such register\n"; continue; }
				auto itA = itR->second.find(a);
				if (itA == itR->second.end()) { std::cout << "No such address\n"; continue; }

				// (Optional but recommended) resolve @file(...) and cross-bank refs before running:
				std::unordered_set<std::string> visited;
				Resolver R(cfg, ws);
				std::string expanded = R.resolve(itA->second, *current, visited);

				scripted_exec::ExecManager EM; // out: files/out/exec/
				std::string stdin_json = (tok.size() >= 4) ? tok[3] : std::string("{}");
				auto res = EM.build_and_run(expanded, stdin_json);

				std::cout << "exit=" << res.exit_code << "\n";
				if (!res.stdout_json.empty()) std::cout << "stdout=" << res.stdout_json << "\n";
				if (!res.stderr_text.empty()) std::cout << "stderr=\n" << res.stderr_text << "\n";
			}
            if (tok[0]==":switch" && tok.size()>=2){
                string name = tok[1]; if (name.size()>4 && name.ends_with(".txt")) name = name.substr(0,name.size()-4);
                string token = (name[0]==cfg.prefix)? name.substr(1): name;
                long long id; if (!parseIntBase(token, cfg.base, id)){ std::cout<<"Bad id\n"; continue; }
                if (!ws.banks.count(id)){
                    string status; if (!openCtx(cfg, ws, name, status)){ std::cout<<status<<"\n"; continue; }
                }
                current = id; std::cout<<"Switched to "<<name<<"\n"; continue;
            }
            if (tok[0]==":ins" && tok.size()>=3){
                string value; for (size_t i=2;i<tok.size();++i){ if (i>2) value.push_back(' '); value+=tok[i]; }
                insert(tok[1], value); continue;
            }
            if (tok[0]==":insr" && tok.size()>=4){
                string value; for (size_t i=3;i<tok.size();++i){ if (i>3) value.push_back(' '); value += tok[i]; }
                insertR(tok[1], tok[2], value); continue;
            }
            if (tok[0]==":del" && tok.size()>=2){ del(tok[1]); continue; }
            if (tok[0]==":delr" && tok.size()>=3){ delR(tok[1], tok[2]); continue; }
            if (tok[0]==":r" && tok.size()>=2){ readMerge(tok[1]); continue; }
            if (tok[0]==":set" && tok.size()>=2){
                if (tok[1]=="prefix" && tok.size()>=3){ cfg.prefix = tok[2][0]; saveCfg(); std::cout<<"prefix="<<cfg.prefix<<"\n"; }
                else if (tok[1]=="base" && tok.size()>=3){ int b=std::stoi(tok[2]); if (b<2||b>36) std::cout<<"base 2..36\n"; else { cfg.base=b; saveCfg(); std::cout<<"base="<<cfg.base<<"\n"; } }
                else if (tok[1]=="widths"){
                    for(size_t i=2;i<tok.size();++i){
                        auto p=tok[i].find('='); if(p==string::npos) continue;
                        auto k=tok[i].substr(0,p); auto v=tok[i].substr(p+1); int n=std::stoi(v);
                        if(k=="bank") cfg.widthBank=n; else if(k=="addr") cfg.widthAddr=n; else if(k=="reg") cfg.widthReg=n;
                    }
                    saveCfg(); std::cout<<"widths bank="<<cfg.widthBank<<" reg="<<cfg.widthReg<<" addr="<<cfg.widthAddr<<"\n";
                }
                else std::cout<<"Unknown :set option\n";
                continue;
            }

            std::cout<<"Unknown command. :help\n";
        }
        std::cout<<"bye.\n";
    }
};

int main(){
    Editor ed;
    ed.repl();
    return 0;
}
