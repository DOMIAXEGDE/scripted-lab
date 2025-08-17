// scripted_exec.hpp (project-capable)
// Header-only execution module for the "scripted" stack.
// Adds multi-file/project support via doc.files[] while keeping single-file mode intact.
// Requirements doc block: /*---DOC--- {json...} ---END---*/
// Supported languages: c, cpp, java, python (entry: "stdio-json")
// Artifacts under files/out/exec/<object>_<hash>/

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
#include <cstdint>
#include <cstdio>

#ifndef _WIN32
  #include <sys/wait.h>
#endif

namespace scripted_exec {

namespace fs = std::filesystem;
using str = std::string;

// ----------------------------- Utilities -----------------------------
inline void ensure_dir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
}

inline void write_file(const fs::path& p, std::string_view data){
    ensure_dir(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
}

inline str read_file(const fs::path& p){
    std::ifstream i(p, std::ios::binary);
    std::ostringstream ss;
    ss << i.rdbuf();
    return ss.str();
}

inline str now_utc_iso() {
    using namespace std::chrono;
    auto t = system_clock::now();
    std::time_t tt = system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// small, stable hash
inline str hex_hash(std::string_view s){
    uint64_t h = 1469598103934665603ull; // FNV-1a basis
    for (unsigned char c : s){
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

inline int system_run(const str& cmd){
    int rc = std::system(cmd.c_str());
#ifndef _WIN32
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
#endif
    return rc;
}

// -------------------- Documentation + parsing helpers -----------------
inline std::optional<str> extract_doc_block(std::string_view code) {
    // Raw with custom delimiter and [\s\S] to match newlines
    static const std::regex r(R"sc(/\*---DOC---([\s\S]*?)---END---\*/)sc");
    std::cmatch m;
    if (std::regex_search(code.begin(), code.end(), m, r)) {
        return str(m[1].first, m[1].second);
    }
    return std::nullopt;
}

inline str trim(str s){
    auto issp = [](unsigned char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    while (!s.empty() && issp((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && issp((unsigned char)s.back())) s.pop_back();
    return s;
}

struct Doc {
    struct ExtraFile {
        str name;     // path relative to work root
        str content;  // file body (optional if ref is given)
        str ref;      // unresolved reference (optional)
    };
    struct Build {
        str cflags;
        str ldflags;
        str classpath;
        str venv;               // python venv dir name (optional)
        std::vector<str> pyreq; // python requirements
    };

    // required
    str object;     // artifact name
    str language;   // "c" | "cpp" | "java" | "python"
    str summary;    // description
    str entry;      // must be "stdio-json"

    // optional
    str main_sym = "main";   // java main class (FQCN) or c/cpp main symbol
    int timeout_ms = 0;      // reserved

    // extras
    std::vector<str> deps;        // simple string deps
    std::vector<ExtraFile> files; // extra files to materialize
    Build build;
    str raw_json;                 // raw doc json snippet
};

inline std::optional<str> find_string_value(const str& j, const str& key){
    // "key"\s*:\s*"([^"]*)"
    std::regex rx(str(R"sc(")sc") + key + str(R"sc("\s*:\s*"([^"]*)")sc"));
    std::smatch m;
    if (std::regex_search(j, m, rx)) return m[1].str();
    return std::nullopt;
}

inline std::vector<str> parse_string_array(const str& j, const str& key){
    std::vector<str> out;
    // "<key>" : [ ... ]   with any content including newlines
    std::regex r_arr(str(R"sc(")sc") + key + str(R"sc("\s*:\s*\[([\s\S]*?)\])sc"));
    std::smatch m;
    if (std::regex_search(j, m, r_arr)){
        str inside = m[1].str();
        std::regex r_item(R"sc("([^"]*)")sc");
        for (auto it = std::sregex_iterator(inside.begin(), inside.end(), r_item);
             it != std::sregex_iterator(); ++it){
            out.push_back((*it)[1].str());
        }
    }
    return out;
}

// files: [ { "name":"...", "content":"..." }, ... ]  (ref optional)
inline std::vector<Doc::ExtraFile> parse_files(const str& j){
    std::vector<Doc::ExtraFile> out;
    std::regex rfiles(R"sc("files"\s*:\s*\[([\s\S]*?)\])sc");
    std::smatch m;
    if (!std::regex_search(j, m, rfiles)) return out;
    str arr = m[1].str();

    // iterate over { ... } objects
    std::regex robj(R"sc(\{([\s\S]*?)\})sc");
    for (auto it = std::sregex_iterator(arr.begin(), arr.end(), robj);
         it != std::sregex_iterator(); ++it) {
        str o = (*it)[1].str();
        Doc::ExtraFile ef;
        std::smatch mm;
        if (std::regex_search(o, mm, std::regex(R"sc("name"\s*:\s*"([^"]*)")sc")))    ef.name = mm[1].str();
        if (std::regex_search(o, mm, std::regex(R"sc("content"\s*:\s*"([^"]*)")sc"))) ef.content = mm[1].str();
        if (std::regex_search(o, mm, std::regex(R"sc("ref"\s*:\s*"([^"]*)")sc")))     ef.ref = mm[1].str();
        if (!ef.name.empty()) out.push_back(std::move(ef));
    }
    return out;
}

inline std::vector<str> parse_pyreq(const str& j){
    std::vector<str> out;
    std::smatch m;
    if (std::regex_search(j, m, std::regex(R"sc("python_requirements"\s*:\s*\[([\s\S]*?)\])sc"))) {
        std::string inside = m[1].str();
        std::regex rstr(R"sc("([^"]*)")sc");
        for (auto it = std::sregex_iterator(inside.begin(), inside.end(), rstr);
             it != std::sregex_iterator(); ++it)
            out.push_back((*it)[1].str());
    }
    return out;
}

inline bool parse_doc_minimal(const str& j_in, Doc& d){
    str j = trim(j_in);
    d.raw_json = j;

    auto getS = [&](const char* k)->std::optional<str>{
        return find_string_value(j, k);
    };

    if (auto v=getS("object"))   d.object   = *v; else return false;
    if (auto v=getS("language")) d.language = *v; else return false;
    if (auto v=getS("summary"))  d.summary  = *v; else return false;
    if (auto v=getS("entry"))    d.entry    = *v; else return false;

    d.main_sym = getS("main").value_or("main");

    // optional timeout
    std::smatch mt;
    std::regex rti(R"sc("timeout_ms"\s*:\s*([0-9]+))sc");
    if (std::regex_search(j, mt, rti)) d.timeout_ms = std::stoi(mt[1].str());

    // arrays/objects
    d.deps  = parse_string_array(j, "deps");
    d.files = parse_files(j);

    // build.*
    d.build.cflags    = getS("cflags").value_or("");
    d.build.ldflags   = getS("ldflags").value_or("");
    d.build.classpath = getS("classpath").value_or("");
    d.build.venv      = getS("venv").value_or("");
    d.build.pyreq     = parse_pyreq(j);

    return true;
}

// --------------------------- Result + manifest ------------------------
struct ExecResult {
    int exit_code = -1;
    str stdout_json;
    str stderr_text;
    fs::path workdir;
    fs::path exe_path;
};

struct ManifestEntry {
    str object;
    str language;
    str hash;
    str created_utc;
    str summary;
    str path;
};

inline void append_manifest(const fs::path& root, const ManifestEntry& e){
    ensure_dir(root);
    auto p = root / "manifest.tsv";
    std::ofstream o(p, std::ios::app);
    o << e.object << '\t' << e.language << '\t' << e.hash << '\t'
      << e.created_utc << '\t' << e.summary << '\t' << e.path << "\n";
}

// ------------------------------- Exec ---------------------------------
struct ExecManager {
    struct Tools {
        str gcc    = std::getenv("SC_GCC")    ? std::getenv("SC_GCC")    : "gcc";
        str gxx    = std::getenv("SC_GXX")    ? std::getenv("SC_GXX")    : "g++";
        str javac  = std::getenv("SC_JAVAC")  ? std::getenv("SC_JAVAC")  : "javac";
        str java   = std::getenv("SC_JAVA")   ? std::getenv("SC_JAVA")   : "java";
        str python = std::getenv("SC_PYTHON") ? std::getenv("SC_PYTHON") : "python3";
        str pip    = std::getenv("SC_PIP")    ? std::getenv("SC_PIP")    : "pip3";
    } tools;

    fs::path out_root;

    explicit ExecManager(fs::path out = fs::path("files")/ "out" / "exec")
      : out_root(std::move(out)) {}

    ExecResult build_and_run(std::string_view code, const str& stdin_json){
        ExecResult R;

        auto doc_str = extract_doc_block(code);
        if (!doc_str){
            R.exit_code = 9001;
            R.stderr_text = "Missing or malformed documentation block.";
            return R;
        }

        Doc d;
        if (!parse_doc_minimal(*doc_str, d)){
            R.exit_code = 9002;
            R.stderr_text = "Doc block missing required fields (object, language, summary, entry).";
            return R;
        }

        if (d.entry != "stdio-json"){
            R.exit_code = 9003;
            R.stderr_text = "Unsupported entry: " + d.entry + " (only stdio-json supported).";
            return R;
        }

        str code_s(code);
        str h = hex_hash(code_s);

        fs::path work = out_root / (d.object + "_" + h);
        ensure_dir(work);
        R.workdir = work;

        write_file(work / "stdin.json", stdin_json);
        write_file(work / "doc.json", d.raw_json);

        // Primary source by language
        fs::path prim;
        if (d.language == "python")      prim = work / "main.py";
        else if (d.language == "java")   prim = work / (d.main_sym + ".java");
        else if (d.language == "c")      prim = work / "main.c";
        else /* cpp default */           prim = work / "main.cpp";
        write_file(prim, code_s);

        // Extra files
        for (const auto& ef : d.files){
            fs::path p = work / ef.name;
            if (!ef.content.empty()){
                write_file(p, ef.content);
            } else {
                write_file(p, str("// unresolved ref: ") + ef.ref + "\n");
            }
        }

        ExecResult X;
        if (d.language == "python"){
            X = run_python(d, prim, work);
        } else if (d.language == "java"){
            X = build_and_run_java(d, prim, work);
        } else if (d.language == "c"){
            X = build_and_run_c(d, prim, work);
        } else if (d.language == "cpp" || d.language == "c++" || d.language == "cplusplus"){
            X = build_and_run_cpp(d, prim, work);
        } else {
            R.exit_code = 9004; R.stderr_text = "Unknown language: " + d.language; return R;
        }

        append_manifest(out_root, ManifestEntry{
            d.object, d.language, h, now_utc_iso(), d.summary, X.exe_path.string()
        });

        return X;
    }

private:
    ExecResult run_python(const Doc& d, const fs::path& main_py, const fs::path& work){
        ExecResult R; R.workdir = work;

        // optional requirements/venv
        if (!d.build.pyreq.empty()){
            fs::path venv = work / (d.build.venv.empty()? "venv" : d.build.venv);
#ifdef _WIN32
            fs::path pybin  = venv / "Scripts" / "python.exe";
            fs::path pipbin = venv / "Scripts" / "pip.exe";
#else
            fs::path pybin  = venv / "bin" / "python";
            fs::path pipbin = venv / "bin" / "pip";
#endif
            if (!fs::exists(pybin)){
                str cmd = tools.python + " -m venv " + venv.string()
                          + " 2>\"" + (work/"venv_stderr.txt").string() + "\"";
                int rc = system_run(cmd);
                if (rc != 0){
                    R.exit_code = rc;
                    R.stderr_text = read_file(work/"venv_stderr.txt");
                    return R;
                }
            }
            // requirements.txt
            std::ostringstream req;
            for (auto& r : d.build.pyreq) req << r << "\n";
            write_file(work/"requirements.txt", req.str());

            // install
            str cmdi = pipbin.string() + " install -r " + (work/"requirements.txt").string()
                       + " 1>\"" + (work/"pip_stdout.txt").string()
                       + "\" 2>\"" + (work/"pip_stderr.txt").string() + "\"";
            int rci = system_run(cmdi);
            if (rci != 0){
                R.exit_code = rci;
                R.stderr_text = read_file(work/"pip_stderr.txt");
                return R;
            }
        }

#ifdef _WIN32
        fs::path py = fs::exists(work/"venv"/"Scripts"/"python.exe") ? (work/"venv"/"Scripts"/"python.exe") : fs::path(tools.python);
#else
        fs::path py = fs::exists(work/"venv"/"bin"/"python") ? (work/"venv"/"bin"/"python") : fs::path(tools.python);
#endif

        fs::path stdout_p = work/"stdout.json";
        fs::path stderr_p = work/"stderr.txt";
        str cmd = str("\"") + py.string() + "\" \"" + main_py.string() + "\""
                + " < \"" + (work/"stdin.json").string() + "\""
                + " 1> \"" + stdout_p.string() + "\""
                + " 2> \"" + stderr_p.string() + "\"";
        R.exit_code = system_run(cmd);
        R.stdout_json = read_file(stdout_p);
        R.stderr_text = read_file(stderr_p);
        R.exe_path = py;
        return R;
    }

    ExecResult build_and_run_cpp(const Doc& d, const fs::path& /*primary*/, const fs::path& work){
        ExecResult R; R.workdir = work;
        fs::path exe = work/"a.out";

        std::vector<fs::path> srcs;
        for (auto& e : fs::directory_iterator(work)){
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c")
                srcs.push_back(e.path());
        }

        std::ostringstream ss;
        ss << "\"" << tools.gxx << "\" " << d.build.cflags << " ";
        for (auto& s : srcs) ss << "\"" << s.string() << "\" ";
        ss << d.build.ldflags << " -o \"" << exe.string()
           << "\" 1>\"" << (work/"compile_stdout.txt").string()
           << "\" 2>\"" << (work/"compile_stderr.txt").string() << "\"";

        int rc = system_run(ss.str());
        if (rc != 0){
            R.exit_code = rc;
            R.stderr_text = read_file(work/"compile_stderr.txt");
            return R;
        }

        fs::path stdout_p = work/"stdout.json";
        fs::path stderr_p = work/"stderr.txt";
        str run = str("\"") + exe.string() + "\""
                + " < \"" + (work/"stdin.json").string() + "\""
                + " 1> \"" + stdout_p.string() + "\""
                + " 2> \"" + stderr_p.string() + "\"";
        R.exit_code = system_run(run);
        R.stdout_json = read_file(stdout_p);
        R.stderr_text = read_file(stderr_p);
        R.exe_path = exe;
        return R;
    }

    ExecResult build_and_run_c(const Doc& d, const fs::path& /*primary*/, const fs::path& work){
        ExecResult R; R.workdir = work;
        fs::path exe = work/"a.out";

        std::vector<fs::path> srcs;
        for (auto& e : fs::directory_iterator(work)){
            if (!e.is_regular_file()) continue;
            if (e.path().extension() == ".c") srcs.push_back(e.path());
        }

        std::ostringstream ss;
        ss << "\"" << tools.gcc << "\" " << d.build.cflags << " ";
        for (auto& s : srcs) ss << "\"" << s.string() << "\" ";
        ss << d.build.ldflags << " -o \"" << exe.string()
           << "\" 1>\"" << (work/"compile_stdout.txt").string()
           << "\" 2>\"" << (work/"compile_stderr.txt").string() << "\"";

        int rc = system_run(ss.str());
        if (rc != 0){
            R.exit_code = rc;
            R.stderr_text = read_file(work/"compile_stderr.txt");
            return R;
        }

        fs::path stdout_p = work/"stdout.json";
        fs::path stderr_p = work/"stderr.txt";
        str run = str("\"") + exe.string() + "\""
                + " < \"" + (work/"stdin.json").string() + "\""
                + " 1> \"" + stdout_p.string() + "\""
                + " 2> \"" + stderr_p.string() + "\"";
        R.exit_code = system_run(run);
        R.stdout_json = read_file(stdout_p);
        R.stderr_text = read_file(stderr_p);
        R.exe_path = exe;
        return R;
    }

    ExecResult build_and_run_java(const Doc& d, const fs::path& primary, const fs::path& work){
        ExecResult R; R.workdir = work;

        // Compile to work/
        str cp = d.build.classpath.empty()? "." : d.build.classpath;
        str cmdc = str("\"") + tools.javac + "\" -cp \"" + cp + "\" -d \"" + work.string() + "\" "
                 + "\"" + primary.string() + "\""
                 + " 1>\"" + (work/"javac_stdout.txt").string()
                 + "\" 2>\"" + (work/"javac_stderr.txt").string() + "\"";
        int rc = system_run(cmdc);
        if (rc != 0){
            R.exit_code = rc;
            R.stderr_text = read_file(work/"javac_stderr.txt");
            return R;
        }

        // Run
        fs::path stdout_p = work/"stdout.json";
        fs::path stderr_p = work/"stderr.txt";
        str cmdr = str("\"") + tools.java + "\" -cp \"" + work.string();
#ifdef _WIN32
        if (!d.build.classpath.empty()) cmdr += str(";") + d.build.classpath;
#else
        if (!d.build.classpath.empty()) cmdr += str(":") + d.build.classpath;
#endif
        cmdr += "\" " + d.main_sym
             + " < \"" + (work/"stdin.json").string() + "\""
             + " 1> \"" + stdout_p.string() + "\""
             + " 2> \"" + stderr_p.string() + "\"";
        R.exit_code = system_run(cmdr);
        R.stdout_json = read_file(stdout_p);
        R.stderr_text = read_file(stderr_p);
        R.exe_path = tools.java;
        return R;
    }
};

} // namespace scripted_exec
