# Exec Add-on (functionality-first, minimal integration)

Files added:
- `scripted_exec.hpp`: header-only execution manager with documentation gate; supports C/C++/Java/Python via JSON stdio.
- `examples/echo.*`: four tiny, documented code objects you can paste into a register value or write to files.

## Minimal integration (CLI)
1) `#include "scripted_exec.hpp"` at the top of `scripted.cpp`.
2) In your REPL command switch, add:
   - `:run_code <reg> <addr>` — fetch the string value at (reg, addr) from the current bank, call `ExecManager::build_and_run`.
   - `:build_code <reg> <addr>` — call the same but ignore stdout; report build status only.
   - `:doc_check <reg> <addr>` — run the doc extractor and print parsed fields or a doc-gate error.
   - `:manifest` — tail `files/out/exec/manifest.jsonl`.

Example (pseudo inside your existing handlers):

```cpp
if (tok[0] == ":run_code" && tok.size() >= 3) {
    long long r,a; parseIntBase(tok[1], cfg.base, r); parseIntBase(tok[2], cfg.base, a);
    auto itR = ws.banks[*current].regs.find(r);
    if (itR == ws.banks[*current].regs.end()) { std::cout << "No such register\n"; continue; }
    auto itA = itR->second.find(a);
    if (itA == itR->second.end()) { std::cout << "No such address\n"; continue; }
    scripted_exec::ExecManager EM; // default out dir: files/out/exec
    std::string input = tok.size()>=4 ? tok[3] : std::string("{}"); // optional stdin json
    auto res = EM.build_and_run(itA->second, input);
    std::cout << "exit=" << res.exit_code << "\n";
    std::cout << "stdout=" << res.stdout_json << "\n";
    if (!res.stderr_text.empty()) std::cout << "stderr=\n" << res.stderr_text << "\n";
}
```

## Minimal integration (Qt)
- Add two menu actions: **Code → Run (stdio-json)** and **Code → Doc check**.
- Wire to presenter methods that read the selected row's value string and call the same `ExecManager` (shared header).

## Documentation-first rule
- Execution is refused unless the top-of-file doc block is present and valid.
- Required fields: `object`, `language`, `summary`, `entry`.
- Current supported `entry`: `stdio-json`.

## Where output lives
- Builds and run artifacts land in `files/out/exec/<object>_<hash>/`.
- `manifest.jsonl` in `files/out/exec/` logs all runs.

## Toolchain override (env):
- `SC_GCC`, `SC_GXX`, `SC_JAVAC`, `SC_JAVA`, `SC_PYTHON`
