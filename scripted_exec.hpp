// scripted_exec.hpp (project-capable)
// Header-only execution module for the "scripted" stack.
// Adds multi-file/project support via doc.files[] while keeping single-file mode intact.
// See README for usage.
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <iomanip>
#include <cstdlib>

namespace scripted_exec {

namespace fs = std::filesystem;
using str = std::string;

// ---------- Utilities -----------------------------------------------------
inline str now_utc_iso() {
    using namespace std::chrono;
    auto t = system_clock::now();
    auto tt = system_clock::to_time_t(t);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    g = *std::gmtime(&tt);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return buf;
}

inline str read_file(const fs::path& p){
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline void write_file(const fs::path& p, std::string_view data){
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
}

inline str shell_escape(std::string_view s){
    // Minimal POSIX shell escape using single quotes.
    std::string out; out.reserve(s.size()+2); out.push_back('\'');
    for (char c : s){ if (c=='\'') out += "'\\''"; else out.push_back(c); }
    out.push_back('\''); return out;
}

// Tiny hashing for cache paths (non-cryptographic)
inline str hex_hash(std::string_view s){
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s){ h ^= c; h *= 1099511628211ULL; }
    std::ostringstream os; os<<std::hex<<std::setw(16)<<std::setfill('0')<<h;
    return os.str();
}

// ---------- Documentation gate -------------------------------------------
// Doc block format: a JSON object wrapped at the top of the code file as:
//
// /*---DOC---
// {
//   "object": "xFFF00.0100.conveyor_tick",
//   "language": "cpp",            // one of: "c","cpp","java","python"
//   "summary": "Moves one item east if space exists.",
//   "entry": "stdio-json",        // I/O contract; current supported: "stdio-json"
//   "main": "main",               // entry symbol or class main (java)
//   "timeout_ms": 2000,           // optional
//   "deps": [],                   // optional logical deps
//   "files": [                    // optional: extra project files (multi-file)
//      {"name":"utils.hpp","content":"..."},
//      {"name":"pkg/mod.py","content":"..."},
//      {"name":"src/Thing.java","content":"..."},
//      {"name":"foo.c","ref":"x00002.01ab"}     // ref => fetch at resolve time
//   ],
//   "build": {                    // optional tool switches
//      "cflags": "-O3 -Wall",
//      "ldflags": "-lpthread",
//      "classpath": "lib/*:bin",
//      "python_requirements": ["numpy"],
//      "venv": "venv"            // create venv in work dir if present
//   }
// }
// ---END---*/
//
// Rule: execution is refused unless required fields present.
// Project mode activates iff "files" exists and is non-empty.
struct Doc {
    str object;
    str language;
    str summary;
    str entry;
    str main_sym;           // "main" or class name for java
    int timeout_ms = 0;     // 0 => no timeout
    std::vector<str> deps;
    struct ExtraFile { str name; str content; str ref; };
    std::vector<ExtraFile> files;
    struct BuildCfg { str cflags, ldflags, classpath, venv; std::vector<str> pyreq; } build;
    str raw_json;
};

inline std::optional<str> extract_doc_block(std::string_view code) {
    size_t limit = std::min<size_t>(code.size(), 8192);
    std::string_view head = code.substr(0, limit);
    size_t start = head.find("/*---DOC---");
    if (start == std::string_view::npos) return std::nullopt;
    size_t json_start = head.find('{', start);
    size_t end_marker = head.find("---END---*/", (json_start==std::string_view::npos? start : json_start));
    if (json_start == std::string_view::npos || end_marker == std::string_view::npos) return std::nullopt;
    std::string_view payload = head.substr(json_start, end_marker - json_start);
    return std::string(payload);
}

inline std::vector<Doc::ExtraFile> parse_files(const str& j){
    std::vector<Doc::ExtraFile> out;
    std::regex rfiles(R"("files"\s*:\s*\[(.*)\])", std::regex::dotall);
    std::smatch m;
    if (!std::regex_search(j, m, rfiles)) return out;
    std::string inside = m[1].str();
    // crude split objects: find {"name": ... } blocks
    std::regex robj(R"(\{[^}]*\})");
    for (auto it = std::sregex_iterator(inside.begin(), inside.end(), robj); it!=std::sregex_iterator(); ++it){
        std::string o = (*it)[0].str();
        Doc::ExtraFile ef;
        std::smatch mm;
        if (std::regex_search(o, mm, std::regex(R"("name"\s*:\s*"([^"]*)")"))) ef.name = mm[1].str();
        if (std::regex_search(o, mm, std::regex(R"("content"\s*:\s*"([^"]*)")"))) ef.content = mm[1].str();
        if (std::regex_search(o, mm, std::regex(R"("ref"\s*:\s*"([^"]*)")"))) ef.ref = mm[1].str();
        if (!ef.name.empty()) out.push_back(std::move(ef));
    }
    return out;
}

inline std::vector<str> parse_pyreq(const str& j){
    std::vector<str> out;
    std::smatch m;
    if (std::regex_search(j, m, std::regex(R"("python_requirements"\s*:\s*\[(.*?)\])", std::regex::dotall))){
        std::string inside = m[1].str();
        std::regex rstr(R"("([^"]*)")");
        for (auto it = std::sregex_iterator(inside.begin(), inside.end(), rstr); it!=std::sregex_iterator(); ++it)
            out.push_back((*it)[1].str());
    }
    return out;
}

inline bool parse_doc_minimal(const str& j, Doc& d){
    auto getS = [&](const char* k)->std::optional<str>{
        std::regex rx(std::string(R"(")") + k + R"("\s*:\s*")" + R"(([^"]*))" + R"("))";
        std::smatch m;
        if (std::regex_search(j, m, rx)) return m[1].str();
        return std::nullopt;
    };
    d.raw_json = j;
    if (auto v=getS("object")) d.object=*v; else return false;
    if (auto v=getS("language")) d.language=*v; else return false;
    if (auto v=getS("summary")) d.summary=*v; else return false;
    if (auto v=getS("entry")) d.entry=*v; else return false;
    d.main_sym = getS("main").value_or("main");
    // timeout_ms (optional int)
    std::regex rti(R"("timeout_ms"\s*:\s*([0-9]+))");
    std::smatch mt;
    if (std::regex_search(j, mt, rti)) d.timeout_ms = std::stoi(mt[1].str());
    // deps[]
    std::regex rdeps(R"("deps"\s*:\s*\[(.*?)\])", std::regex::dotall);
    if (std::regex_search(j, mt, rdeps)){
        std::string inside = mt[1].str();
        std::regex rstr(R"("([^"]*)")");
        for (auto it = std::sregex_iterator(inside.begin(), inside.end(), rstr); it!=std::sregex_iterator(); ++it)
            d.deps.push_back((*it)[1].str());
    }
    d.files = parse_files(j);
    d.build.cflags = getS("cflags").value_or("");
    d.build.ldflags = getS("ldflags").value_or("");
    d.build.classpath = getS("classpath").value_or("");
    d.build.venv = getS("venv").value_or("");
    d.build.pyreq = parse_pyreq(j);
    return true;
}

// ---------- Manifest ------------------------------------------------------
struct ManifestEntry {
    str object;
    str language;
    str hash;
    str created_utc;
    str summary;
    str path;
};

inline void append_manifest(const fs::path& root, const ManifestEntry& e){
    fs::create_directories(root);
    fs::path jf = root / "manifest.jsonl";
    std::ofstream f(jf, std::ios::app);
    f << "{"
      << "\"object\":\"" << e.object << "\","
      << "\"language\":\"" << e.language << "\","
      << "\"hash\":\"" << e.hash << "\","
      << "\"created_utc\":\"" << e.created_utc << "\","
      << "\"summary\":" << std::quoted(e.summary) << ","
      << "\"path\":" << std::quoted(e.path) 
      << "}\n";
}

// ---------- Execution -----------------------------------------------------
struct ExecResult {
    int exit_code = -1;
    str stdout_json;
    str stderr_text;
    fs::path workdir;
    fs::path exe_path;
};

inline int system_run(const str& cmd){
#ifdef _WIN32
    int rc = std::system(cmd.c_str());
    return rc;
#else
    int rc = std::system(cmd.c_str());
    if (rc == -1) return rc;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return rc;
#endif
}

struct Toolchain {
    str gcc = std::getenv("SC_GCC")? std::getenv("SC_GCC") : "gcc";
    str gxx = std::getenv("SC_GXX")? std::getenv("SC_GXX") : "g++";
    str javac = std::getenv("SC_JAVAC")? std::getenv("SC_JAVAC") : "javac";
    str java = std::getenv("SC_JAVA")? std::getenv("SC_JAVA") : "java";
    str python = std::getenv("SC_PYTHON")? std::getenv("SC_PYTHON") : "python3";
    str pip = std::getenv("SC_PIP")? std::getenv("SC_PIP") : "pip3";
};

struct ExecManager {
    fs::path out_root;
    Toolchain tools;

    explicit ExecManager(fs::path out = fs::path("files")/ "out" / "exec")
        : out_root(std::move(out)) {}

    ExecResult build_and_run(std::string_view code, const str& stdin_json){
        ExecResult R;

        auto doc_str = extract_doc_block(code);
        if (!doc_str) {
            R.stderr_text = "Missing or malformed documentation block.";
            R.exit_code = 9001; return R;
        }
        Doc d;
        if (!parse_doc_minimal(*doc_str, d)){
            R.stderr_text = "Doc block missing required fields (object, language, summary, entry).";
            R.exit_code = 9002; return R;
        }
        if (d.entry != "stdio-json"){
            R.stderr_text = "Unsupported entry: " + d.entry + " (only stdio-json supported).";
            R.exit_code = 9003; return R;
        }

        str code_s(code);
        str h = hex_hash(code_s);
        fs::path work = out_root / (d.object + "_" + h);
        fs::create_directories(work);
        R.workdir = work;

        write_file(work / "stdin.json", stdin_json);
        write_file(work / "doc.json", d.raw_json);

        // Write primary source (serves as main entry)
        fs::path prim;
        if (d.language == "python") prim = work / "main.py";
        else if (d.language == "java") prim = work / (d.main_sym + ".java");
        else if (d.language == "c") prim = work / "main.c";
        else prim = work / "main.cpp";
        write_file(prim, code_s);

        // Write extra files if project mode is used
        for (auto& ef : d.files){
            fs::path p = work / ef.name;
            if (!ef.content.empty())
                write_file(p, ef.content);
            else if (!ef.ref.empty()) {
                // In-world reference: store a placeholder note—caller should have pre-resolved content.
                // We still write a file with ref marker to make it obvious.
                write_file(p, std::string("// unresolved ref: ") + ef.ref + "\n");
            }
        }

        // Optional Python venv + requirements
        if (d.language == "python" && (!d.build.venv.empty() || !d.build.pyreq.empty())){
            fs::path venv = work / (d.build.venv.empty()? "venv" : d.build.venv);
            if (!fs::exists(venv / "bin" / "python") && !fs::exists(venv / "Scripts" / "python.exe")){
#ifdef _WIN32
                system_run("python -m venv " + (venv.string()));
#else
                system_run("python3 -m venv " + (venv.string()));
#endif
            }
            if (!d.build.pyreq.empty()){
                std::ostringstream req; for (auto& r: d.build.pyreq) req<<r<<"\n";
                write_file(work / "requirements.txt", req.str());
#ifdef _WIN32
                str pip = (venv / "Scripts" / "pip.exe").string();
#else
                str pip = (venv / "bin" / "pip").string();
#endif
                system_run(pip + " install -r " + (work/"requirements.txt").string());
            }
        }

        // Dispatch
        ExecResult X;
        if (d.language == "cpp"){
            X = build_and_run_cpp(d, prim, work);
        } else if (d.language == "c"){
            X = build_and_run_c(d, prim, work);
        } else if (d.language == "java"){
            X = build_and_run_java(d, prim, work);
        } else if (d.language == "python"){
            X = run_python(d, prim, work);
        } else {
            R.exit_code = 9004; R.stderr_text = "Unknown language: " + d.language; return R;
        }

        append_manifest(out_root, ManifestEntry{ d.object, d.language, h, now_utc_iso(), d.summary, X.exe_path.string() });
        return X;
    }

    ExecResult run_python(const Doc& d, const fs::path& main_py, const fs::path& work){
        ExecResult R; R.workdir = work;
        fs::path out = work / "stdout.json";
        fs::path err = work / "stderr.txt";
        // Prefer venv if present
#ifdef _WIN32
        fs::path py = fs::exists(work/"venv"/"Scripts"/"python.exe") ? work/"venv"/"Scripts"/"python.exe" : tools.python;
#else
        fs::path py = fs::exists(work/"venv"/"bin"/"python") ? work/"venv"/"bin"/"python" : tools.python;
#endif
        str cmd = shell_escape(py.string()) + " " + shell_escape(main_py.string()) +
                  " < " + shell_escape((work/"stdin.json").string()) +
                  " > " + shell_escape(out.string()) +
                  " 2> " + shell_escape(err.string());
        R.exit_code = system_run(cmd);
        R.stdout_json = read_file(out);
        R.stderr_text = read_file(err);
        R.exe_path = main_py;
        return R;
    }

    ExecResult build_and_run_cpp(const Doc& d, const fs::path& primary, const fs::path& work){
        ExecResult R; R.workdir = work;
        // Compile all .cpp in work recursively
        std::vector<fs::path> sources;
        for (auto& p : fs::recursive_directory_iterator(work))
            if (p.is_regular_file() && p.path().extension()==".cpp") sources.push_back(p.path());
        if (sources.empty()) sources.push_back(primary);
        fs::path exe = work / "a.out";
        std::ostringstream cmd;
        cmd << tools.gxx << " -std=c++20 -O2 -pipe ";
        if (!d.build.cflags.empty()) cmd << d.build.cflags << " ";
        for (auto& s : sources) cmd << shell_escape(s.string()) << " ";
        if (!d.build.ldflags.empty()) cmd << d.build.ldflags << " ";
        cmd << "-o " << shell_escape(exe.string());
        int b = system_run(cmd.str());
        fs::path out = work/"stdout.json", err = work/"stderr.txt";
        if (b != 0){
            R.exit_code = 100+b;
            write_file(err, "Build failed (C++). Command:\n"+cmd.str()+"\n");
            R.stderr_text = read_file(err);
            R.exe_path = exe; return R;
        }
        str cmd_run = shell_escape(exe.string()) + " < " + shell_escape((work/"stdin.json").string()) + " > " + shell_escape(out.string()) + " 2> " + shell_escape(err.string());
        R.exit_code = system_run(cmd_run);
        R.stdout_json = read_file(out);
        R.stderr_text = read_file(err);
        R.exe_path = exe;
        return R;
    }

    ExecResult build_and_run_c(const Doc& d, const fs::path& primary, const fs::path& work){
        ExecResult R; R.workdir = work;
        // Compile all .c in work recursively
        std::vector<fs::path> sources;
        for (auto& p : fs::recursive_directory_iterator(work))
            if (p.is_regular_file() && p.path().extension()==".c") sources.push_back(p.path());
        if (sources.empty()) sources.push_back(primary);
        fs::path exe = work / "a.out";
        std::ostringstream cmd;
        cmd << tools.gcc << " -std=c17 -O2 -pipe ";
        if (!d.build.cflags.empty()) cmd << d.build.cflags << " ";
        for (auto& s : sources) cmd << shell_escape(s.string()) << " ";
        if (!d.build.ldflags.empty()) cmd << d.build.ldflags << " ";
        cmd << "-o " << shell_escape(exe.string());
        int b = system_run(cmd.str());
        fs::path out = work/"stdout.json", err = work/"stderr.txt";
        if (b != 0){
            R.exit_code = 200+b;
            write_file(err, "Build failed (C). Command:\n"+cmd.str()+"\n");
            R.stderr_text = read_file(err);
            R.exe_path = exe; return R;
        }
        str cmd_run = shell_escape(exe.string()) + " < " + shell_escape((work/"stdin.json").string()) + " > " + shell_escape(out.string()) + " 2> " + shell_escape(err.string());
        R.exit_code = system_run(cmd_run);
        R.stdout_json = read_file(out);
        R.stderr_text = read_file(err);
        R.exe_path = exe;
        return R;
    }

    ExecResult build_and_run_java(const Doc& d, const fs::path& primary, const fs::path& work){
        ExecResult R; R.workdir = work;
        fs::path srcDir = work; // we wrote .java files in work (and subdirs)
        fs::path binDir = work / "bin";
        fs::create_directories(binDir);
        // compile all java
        std::ostringstream cmd;
        cmd << tools.javac << " -d " << shell_escape(binDir.string()) << " ";
        // classpath for compile
        if (!d.build.classpath.empty()) cmd << "-cp " << shell_escape(d.build.classpath) << " ";
        std::vector<fs::path> sources;
        for (auto& p : fs::recursive_directory_iterator(srcDir))
            if (p.is_regular_file() && p.path().extension()==".java") sources.push_back(p.path());
        if (sources.empty()) sources.push_back(primary);
        for (auto& s : sources) cmd << shell_escape(s.string()) << " ";
        int b = system_run(cmd.str());
        fs::path out = work/"stdout.json", err = work/"stderr.txt";
        if (b != 0){
            R.exit_code = 300+b;
            write_file(err, "Build failed (Java). Command:\n"+cmd.str()+"\n");
            R.stderr_text = read_file(err);
            return R;
        }
        // run
        std::ostringstream run;
        run << tools.java << " -cp ";
        if (!d.build.classpath.empty()) run << shell_escape(d.build.classpath + fs::path::preferred_separator + binDir.string()) << " ";
        else run << shell_escape(binDir.string()) << " ";
        run << d.main_sym
            << " < " << shell_escape((work/"stdin.json").string())
            << " > " << shell_escape(out.string())
            << " 2> " << shell_escape(err.string());
        R.exit_code = system_run(run.str());
        R.stdout_json = read_file(out);
        R.stderr_text = read_file(err);
        R.exe_path = binDir / (d.main_sym + ".class");
        return R;
    }
};

} // namespace scripted_exec
