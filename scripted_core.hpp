// scripted_core.hpp
// Header-only shared backend for scripted CLI & GUI.
// C++23, no external deps. All I/O under files/ and files/out/.
// --- platform detection (scripted_core.hpp) -------------------------------
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>

namespace scripted {

namespace fs = std::filesystem;
using std::string;

#if defined(_WIN32) || defined(_WIN64)
    inline constexpr bool kWindows = true;
    inline constexpr bool kLinux   = false;
    inline const char* platformName() { return "Windows"; }
#else
    inline constexpr bool kWindows = false;
    #if defined(__linux__)
        inline constexpr bool kLinux = true;
        inline const char* platformName() { return "Linux"; }
    #else
        inline constexpr bool kLinux = false;
        inline const char* platformName() { return "Unknown"; }
    #endif
#endif

// Optional: detect WSL for friendlier messages
inline bool isWSL() {
#if defined(__linux__)
    if (const char* e = std::getenv("WSL_DISTRO_NAME")) return true;
    std::ifstream f("/proc/version");
    std::string s( (std::istreambuf_iterator<char>(f)), {} );
    return s.find("Microsoft") != std::string::npos || s.find("WSL") != std::string::npos;
#else
    return false;
#endif
}

// Normalize line endings: choose '\n' everywhere (safer across OSes)
inline constexpr const char* kEOL = "\n";

inline string trim(string s) {
    auto notspace = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}
inline int digitValue(char c){
    if (c>='0' && c<='9') return c-'0';
    if (c>='A' && c<='Z') return 10+(c-'A');
    if (c>='a' && c<='z') return 10+(c-'a');
    return -1;
}
inline bool parseIntBase(const string& s, int base, long long& out){
    if (s.empty()) return false;
    long long v=0;
    for (char c: s){
        int d = digitValue(c);
        if (d<0 || d>=base) return false;
        v = v*base + d;
        if (v < 0) return false;
    }
    out = v;
    return true;
}
inline string toBaseN(long long val, int base, int width){
    if (base<2 || base>36) base=10;
    if (val==0) return string(std::max(1,width),'0');
    bool neg = val<0; if (neg) val = -val;
    string s;
    while (val>0){
        int d = int(val%base);
        s.push_back(d<10? char('0'+d) : char('a'+(d-10)));
        val/=base;
    }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    if (width>0 && (int)s.size()<width) s = string(width - (int)s.size(),'0') + s;
    return s;
}

// ----------------------------- Config/Paths/Model -----------------------------
struct Config {
    char prefix = 'x';
    int  base = 10;
    int  widthBank = 5;
    int  widthReg  = 2;
    int  widthAddr = 4;

    string toJSON() const {
        std::ostringstream os;
        os << "{\n";
        os << "  \"prefix\": \"" << prefix << "\",\n";
        os << "  \"base\": " << base << ",\n";
        os << "  \"widthBank\": " << widthBank << ",\n";
        os << "  \"widthReg\": " << widthReg << ",\n";
        os << "  \"widthAddr\": " << widthAddr << "\n";
        os << "}\n";
        return os.str();
    }
    static Config fromJSON(const string& j){
        Config c;
        auto getStr=[&](const string& key, const string& def)->string{
            auto p = j.find("\""+key+"\"");
            if (p==string::npos) return def;
            p = j.find(':', p); if (p==string::npos) return def;
            p = j.find('"', p); if (p==string::npos) return def;
            auto q = j.find('"', p+1);
            if (q==string::npos) return def;
            return j.substr(p+1, q-(p+1));
        };
        auto getInt=[&](const string& key, int def)->int{
            auto p = j.find("\""+key+"\"");
            if (p==string::npos) return def;
            p = j.find(':', p); if (p==string::npos) return def;
            auto q = j.find_first_of(",\n}", p+1);
            string num = trim(j.substr(p+1, q-(p+1)));
            try { return std::stoi(num); } catch(...) { return def; }
        };
        string pf = getStr("prefix", "\"x\"");
        if (pf.size()>=2) c.prefix = pf[1];
        c.base       = getInt("base", 10);
        c.widthBank  = getInt("widthBank", 5);
        c.widthReg   = getInt("widthReg", 2);
        c.widthAddr  = getInt("widthAddr", 4);
        return c;
    }
};

struct Paths {
    fs::path root   = "files";
    fs::path outdir = root / "out";
    fs::path config = root / "config.json";
    void ensure() const {
        fs::create_directories(root);
        fs::create_directories(outdir);
    }
};

struct Bank {
    long long id = 0;
    string title;
    // reg -> (addr -> value)
    std::map<long long, std::map<long long, string>> regs;
    bool empty() const {
        if (regs.empty()) return true;
        for (auto& [r, addrs] : regs) if (!addrs.empty()) return false;
        return true;
    }
};

struct Workspace {
    std::map<long long, Bank> banks;       // id -> Bank
    std::map<long long, string> filenames; // id -> path
};

// ----------------------------- Parsing & I/O -----------------------------
struct ParseResult { bool ok=true; string err; };

inline ParseResult parseBankText(const string& text, const Config& cfg, Bank& outBank) {
    std::vector<string> lines;
    {
        std::istringstream is(text);
        string line;
        while (std::getline(is, line)) lines.push_back(line);
    }
    if (lines.empty()) return {false, "empty file"};
    size_t i=0;
    while (i<lines.size() && trim(lines[i]).empty()) i++;
    if (i==lines.size()) return {false, "no header found"};
    string header = trim(lines[i]);

    string headerAccum = header;
    size_t j=i+1;
    while (headerAccum.find('{')==string::npos && j<lines.size()){
        headerAccum += " " + trim(lines[j]);
        j++;
    }
    if (headerAccum.find('{')==string::npos) return {false, "missing '{' after header"};

    size_t lp = headerAccum.find('(');
    size_t rp = headerAccum.rfind(')');
    if (lp==string::npos || rp==string::npos || rp<lp) return {false, "malformed header: parentheses"};
    string left  = trim(headerAccum.substr(0, lp));
    string title = trim(headerAccum.substr(lp+1, rp-lp-1));

    if (!left.empty() && left[0]==cfg.prefix) left = left.substr(1);
    long long bankId;
    if (!parseIntBase(left, cfg.base, bankId)) return {false, "cannot parse bank id"};

    outBank = {};
    outBank.id = bankId;
    outBank.title = title;

    size_t bodyStartLine = i;
    while (bodyStartLine<lines.size() && lines[bodyStartLine].find('{')==string::npos) bodyStartLine++;
    if (bodyStartLine==lines.size()) return {false, "missing body start"};
    bodyStartLine++;

    long long currentReg = 1;
    for (size_t k=bodyStartLine; k<lines.size(); ++k){
        string s = lines[k];
        if (s.find('}')!=string::npos) break;
        if (trim(s).empty()) continue;

        if (!s.empty() && s[0] != '\t'){
            long long regId;
            if (!parseIntBase(trim(s), cfg.base, regId)){
                return {false, "invalid register line: " + trim(s)};
            }
            currentReg = regId;
            continue;
        }
        string t = s;
        while (!t.empty() && (t[0]=='\t' || t[0]==' ')) t.erase(t.begin());
        size_t sep = t.find('\t');
        if (sep==string::npos) sep = t.find(' ');
        string addrTok, val;
        if (sep==string::npos){ addrTok = trim(t); val=""; }
        else { addrTok = trim(t.substr(0, sep)); val = t.substr(sep+1); }

        long long addrId;
        if (!parseIntBase(addrTok, cfg.base, addrId))
            return {false, "invalid address id: " + addrTok};
        outBank.regs[currentReg][addrId] = val;
    }
    return {};
}

inline string writeBankText(const Bank& b, const Config& cfg){
    std::ostringstream os;
    string bankStr = string(1,cfg.prefix) + toBaseN(b.id, cfg.base, cfg.widthBank);
    os << bankStr << "\t(" << b.title << "){\n";
    bool multi = (b.regs.size()>1) || (b.regs.size()==1 && b.regs.begin()->first!=1);
    if (!multi){
        auto it = b.regs.find(1);
        if (it != b.regs.end()){
            for (auto& [aid, val] : it->second){
                os << "\t" << toBaseN(aid, cfg.base, cfg.widthAddr) << "\t" << val << "\n";
            }
        }
    } else {
        for (auto& [rid, addrs] : b.regs){
            os << toBaseN(rid, cfg.base, cfg.widthReg) << "\n";
            for (auto& [aid, val] : addrs){
                os << "\t" << toBaseN(aid, cfg.base, cfg.widthAddr) << "\t" << val << "\n";
            }
        }
    }
    os << "}\n";
    return os.str();
}

inline fs::path contextFileName(const Config& cfg, long long bankId){
    return fs::path("files") / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".txt");
}
inline fs::path outResolvedName(const Config& cfg, long long bankId){
    return fs::path("files/out") / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".resolved.txt");
}
inline fs::path outJsonName(const Config& cfg, long long bankId){
    return fs::path("files/out") / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".json");
}

inline bool loadContextFile(const Config& cfg, const fs::path& file, Bank& bank, string& err){
    if (!fs::exists(file)) { err = "file not found: " + file.string(); return false; }
    std::ifstream in(file, std::ios::binary);
    if (!in){ err="cannot open: " + file.string(); return false; }
    string text( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
    ParseResult pr = parseBankText(text, cfg, bank);
    if (!pr.ok) { err = pr.err; return false; }
    return true;
}
// --- saveContextFile: ensure dirs; write atomically-ish -------------------
inline bool saveContextFile(const Config& cfg,
                            const std::filesystem::path& path,
                            const Bank& b,
                            std::string& err)
{
    try {
        std::filesystem::create_directories(path.parent_path());

        // Write to a temp file first
        auto tmp = path; tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) { err = "Cannot open temp file for write: " + tmp.string(); return false; }
            std::string text = writeBankText(b, cfg);
            out.write(text.data(), (std::streamsize)text.size());
            if (!out) { err = "Write failed: " + tmp.string(); return false; }
        }

        // Replace the target (works across volumes with fallback)
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::copy_file(tmp, path,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp);
            if (ec) { err = "Replace failed: " + path.string() + " (" + ec.message() + ")"; return false; }
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}


inline bool ensureBankLoadedInWorkspace(const Config& cfg, Workspace& ws, long long bankId, string& err){
    if (ws.banks.count(bankId)) return true;
    fs::path file = contextFileName(cfg, bankId);
    if (!fs::exists(file)) { err = "missing context file: " + file.string(); return false; }
    Bank b;
    if (!loadContextFile(cfg, file, b, err)) return false;
    ws.banks[bankId] = std::move(b);
    ws.filenames[bankId] = file.string();
    return true;
}

// ----------------------------- Resolver (both styles active) -----------------------------
struct Resolver {
    const Config& cfg;
    Workspace& ws;
    Resolver(const Config& c, Workspace& w): cfg(c), ws(w) {}

    bool getValue(long long bank, long long reg, long long addr, string& out) const {
        string err;
        (void)ensureBankLoadedInWorkspace(cfg, const_cast<Workspace&>(ws), bank, err);
        auto itB = ws.banks.find(bank);
        if (itB==ws.banks.end()) return false;
        const auto& b = itB->second;
        auto itR = b.regs.find(reg);
        if (itR==b.regs.end()) return false;
        auto itA = itR->second.find(addr);
        if (itA==itR->second.end()) return false;
        out = itA->second;
        return true;
    }
    bool getValueTwoPart(long long bank, long long addr, string& out) const {
        string err;
        (void)ensureBankLoadedInWorkspace(cfg, const_cast<Workspace&>(ws), bank, err);
        return getValue(bank, 1, addr, out);
    }
    string includeFile(const string& name) const {
        fs::path p = fs::path("files") / name;
        if (!fs::exists(p)) return string("[Missing file: ")+name+"]";
        std::ifstream in(p, std::ios::binary);
        if (!in) return string("[Cannot open file: ")+name+"]";
        return string( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
    }

    string resolve(const string& input, long long currentBank, std::unordered_set<string>& visited) const {
        string s = input;
        {   // @file(...)
            static std::regex fileRe(R"(@file\(([^)]+)\))");
            std::smatch m; string out; out.reserve(s.size());
            string::const_iterator searchStart( s.cbegin() ); size_t last = 0;
            while (std::regex_search(searchStart, s.cend(), m, fileRe)) {
                size_t pos = m.position(0) + (searchStart - s.cbegin());
                size_t len = m.length(0);
                out.append(s, last, pos - last);
                string fname = trim(m[1].str());
                out += includeFile(fname);
                searchStart = s.cbegin() + pos + len;
                last = pos + len;
            }
            out.append(s, last, string::npos);
            s.swap(out);
        }
        {   // three-part numeric: b.r.a
            static std::regex tri(R"((\d+)\.(\d+)\.(\d+))");
            std::smatch m; string out; out.reserve(s.size());
            string::const_iterator searchStart( s.cbegin() ); size_t last = 0;
            while (std::regex_search(searchStart, s.cend(), m, tri)) {
                size_t pos = m.position(0) + (searchStart - s.cbegin());
                size_t len = m.length(0);
                out.append(s, last, pos - last);
                long long b = std::stoll(m[1].str());
                long long r = std::stoll(m[2].str());
                long long a = std::stoll(m[3].str());
                string key = std::to_string(b)+"."+std::to_string(r)+"."+std::to_string(a);
                if (visited.count(key)) out += "[Circular Ref: " + m[0].str() + "]";
                else {
                    string v;
                    if (!getValue(b, r, a, v)) out += "[Missing " + m[0].str() + "]";
                    else { auto v2=visited; v2.insert(key); out += resolve(v, b, v2); }
                }
                searchStart = s.cbegin() + pos + len; last = pos + len;
            }
            out.append(s, last, string::npos);
            s.swap(out);
        }
        {   // two-part prefixed: x<bank>.<addr>
            static std::regex two(R"(([A-Za-z])([0-9A-Za-z]+)\.([0-9A-Za-z]+))");
            std::smatch m; string out; out.reserve(s.size());
            string::const_iterator searchStart( s.cbegin() ); size_t last = 0;
            while (std::regex_search(searchStart, s.cend(), m, two)) {
                size_t pos = m.position(0) + (searchStart - s.cbegin());
                size_t len = m.length(0);
                out.append(s, last, pos - last);
                char pf = m[1].str()[0];
                if (pf != cfg.prefix) out += m[0].str();
                else {
                    long long b=0, a=0;
                    if (!parseIntBase(m[2].str(), cfg.base, b) || !parseIntBase(m[3].str(), cfg.base, a)) {
                        out += "[BadRef " + m[0].str() + "]";
                    } else {
                        string key = string(1, pf) + m[2].str() + "." + m[3].str();
                        if (visited.count(key)) out += "[Circular Ref: " + m[0].str() + "]";
                        else {
                            string v;
                            if (!getValueTwoPart(b, a, v)) out += "[Missing " + m[0].str() + "]";
                            else { auto v2=visited; v2.insert(key); out += resolve(v, b, v2); }
                        }
                    }
                }
                searchStart = s.cbegin() + pos + len; last = pos + len;
            }
            out.append(s, last, string::npos);
            s.swap(out);
        }
        return s;
    }
};

// ----------------------------- Config file helpers -----------------------------
inline void ensurePaths(const Paths& P){ P.ensure(); }
inline Config loadConfig(const Paths& P){
    ensurePaths(P);
    Config cfg;
    if (fs::exists(P.config)) {
        std::ifstream in(P.config);
        string j( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
        cfg = Config::fromJSON(j);
    } else {
        std::ofstream out(P.config);
        out << cfg.toJSON();
    }
    return cfg;
}
inline void saveConfig(const Paths& P, const Config& cfg){
    std::ofstream out(P.config);
    out << cfg.toJSON();
}

// ----------------------------- Utility ops used by CLI/GUI -----------------------------
// --- openCtx: load-or-create without testing writability ------------------
inline bool openCtx(const Config& cfg,
                    Workspace& ws,
                    std::string nameOrStem,
                    std::string& status)
{
    // Normalize stem and compute id
    std::string stem = nameOrStem;
    if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".txt")
        stem.resize(stem.size() - 4);

    std::string token = (!stem.empty() && stem[0] == cfg.prefix) ? stem.substr(1) : stem;
    long long id = 0;
    if (!parseIntBase(token, cfg.base, id)) {
        status = "Bad context id: " + stem;
        return false;
    }

    auto path = contextFileName(cfg, id);
    Bank b;

    if (std::filesystem::exists(path)) {
        // OPEN FOR READING ONLY — opening must NOT fail if file is read-only
        std::ifstream in(path, std::ios::binary);
        if (!in) { status = "Cannot open: " + path.string(); return false; }
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto pr = parseBankText(text, cfg, b);
        if (!pr.ok) { status = "Parse failed: " + pr.err; return false; }
        if (b.title.empty()) b.title = stem;
        ws.banks[id] = std::move(b);
        status = "Opened " + path.string();
        return true;
    }

    // New (empty) bank if file doesn't exist
    b.title = stem;
    ws.banks[id] = std::move(b);
    status = "Created new context: " + path.string();
    return true;
}


inline string resolveBankToText(const Config& cfg, Workspace& ws, long long bankId){
    Resolver R(cfg, ws);
    auto& b = ws.banks[bankId];
    std::ostringstream os;
    string bankStr = string(1,cfg.prefix) + toBaseN(b.id, cfg.base, cfg.widthBank);
    os << bankStr << "\t(" << b.title << "){\n";
    for (auto& [rid, addrs] : b.regs){
        if (b.regs.size()>1) os << toBaseN(rid, cfg.base, cfg.widthReg) << "\n";
        for (auto& [aid, val] : addrs){
            std::unordered_set<string> visited;
            string out = R.resolve(val, b.id, visited);
            os << "\t" << toBaseN(aid, cfg.base, cfg.widthAddr) << "\t" << out << "\n";
        }
    }
    os << "}\n";
    return os.str();
}

inline string exportBankToJSON(const Config& cfg, Workspace& ws, long long bankId){
    Resolver R(cfg, ws);
    auto& b = ws.banks[bankId];
    std::ostringstream os;
    os << "{\n";
    os << "  \"bank\": \""<< cfg.prefix<<toBaseN(b.id,cfg.base,cfg.widthBank) <<"\",\n";
    os << "  \"title\": \""<< b.title <<"\",\n";
    os << "  \"registers\": [\n";
    bool firstR=true;
    for (auto& [rid, addrs] : b.regs){
        if (!firstR) os << ",\n"; firstR=false;
        os << "    {\"id\":\""<<toBaseN(rid,cfg.base,cfg.widthReg)<<"\",\"addresses\":[\n";
        bool firstA=true;
        for (auto& [aid, val] : addrs){
            if (!firstA) os << ",\n"; firstA=false;
            std::unordered_set<string> visited;
            string out = R.resolve(val, b.id, visited);
            auto esc = [](const string& s){
                string r; r.reserve(s.size()*11/10 + 8);
                for (char c: s){
                    if (c=='\\' || c=='"') { r.push_back('\\'); r.push_back(c); }
                    else if (c=='\n') { r += "\\n"; }
                    else r.push_back(c);
                }
                return r;
            };
            os << "      {\"id\":\""<<toBaseN(aid,cfg.base,cfg.widthAddr)
               <<"\",\"value\":\""<<esc(out)<<"\"}";
        }
        os << "\n    ]}";
    }
    os << "\n  ]\n";
    os << "}\n";
    return os.str();
}

inline void preloadAll(const Config& cfg, Workspace& ws){
    for (auto& entry : fs::directory_iterator("files")){
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() != ".txt") continue;
        string stem = p.stem().string();
        if (stem.empty() || stem[0]!=cfg.prefix) continue;
        long long id;
        if (!parseIntBase(stem.substr(1), cfg.base, id)) continue;
        string err; (void)ensureBankLoadedInWorkspace(cfg, ws, id, err);
    }
}

} // namespace scripted
