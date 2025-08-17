// scripted-gui.cpp — Win32 GUI using shared core.
// Build (MinGW):
// g++ -std=c++23 -O2 scripted-gui.cpp -o scripted-gui.exe -municode -lgdi32 -lcomctl32 -lcomdlg32 -lole32 -luuid

#if !defined(_WIN32)
  #include <cstdio>
  int main() {
      std::fprintf(stderr, "scripted-gui is Windows-only. Use scripted (CLI) on Linux.\n");
      return 1;
  }
#else

  #ifndef UNICODE
  #define UNICODE
  #endif
  #ifndef _UNICODE
  #define _UNICODE
  #endif

  // --- Windows + std headers (all stay inside the #else) ---
  #include <windows.h>
  #include <commctrl.h>
  #include <commdlg.h>
  #include <shellapi.h>
  #include <uxtheme.h>

  #include <string>
  #include <vector>
  #include <chrono>
  #include <thread>
  #include <optional>
  #include <atomic>

  #include "scripted_core.hpp"

  #ifdef _MSC_VER
  #  pragma comment(lib, "comctl32.lib")
  #endif

  using namespace scripted;

  // ---- your full Windows implementation here ----
  // App struct, helpers, WndProc, wWinMain, etc.
  // (and the openCtxUI()/saveCurrent() replacements from earlier)

// ---------- UTF helpers ----------
static std::wstring s2ws(const std::string& s){
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string ws2s(const std::wstring& w){
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string nowStr(){
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[64]; sprintf(buf, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ---------- IDs ----------
enum : int {
    ID_BANK_COMBO = 1001,
    ID_BTN_SWITCH,
    ID_BTN_PRELOAD,
    ID_BTN_OPEN,
    ID_BTN_SAVE,
    ID_BTN_RESOLVE,
    ID_BTN_EXPORT,
    ID_LIST,
    ID_EDIT_VALUE,
    ID_EDIT_ADDR,
    ID_EDIT_REG,
    ID_BTN_INSERT,
    ID_BTN_DELETE,
    ID_STATUS,
    ID_EDIT_FILTER,
    ID_LOG,
    ID_PROGRESS
};

// Menu/accelerators
enum : int {
    IDM_FILE_OPEN = 2001,
    IDM_FILE_SAVE,
    IDM_FILE_EXIT,
    IDM_VIEW_PRELOAD,
    IDM_VIEW_RELOAD,
    IDM_EDIT_INSERT,
    IDM_EDIT_DELETE,
    IDM_EDIT_COPY,
    IDM_HELP_ABOUT,
    IDM_ACTION_RESOLVE,
    IDM_ACTION_EXPORT,
    IDM_FOCUS_FILTER
};

// Worker messages
enum : UINT {
    WM_APP_RESOLVE_DONE = WM_APP + 1,
    WM_APP_EXPORT_DONE  = WM_APP + 2
};

// ---------- Small helpers ----------
static void copyToClipboard(HWND owner, const std::string& s){
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, s.size()+1);
    if (hMem){
        void* p = GlobalLock(hMem);
        memcpy(p, s.c_str(), s.size()+1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

static HACCEL buildAccelerators(){
    static ACCEL acc[] = {
        { FCONTROL, 'O', IDM_FILE_OPEN },
        { FCONTROL, 'S', IDM_FILE_SAVE },
        { FCONTROL, 'R', IDM_ACTION_RESOLVE },
        { FCONTROL, 'E', IDM_ACTION_EXPORT },
        { FVIRTKEY, VK_F5, IDM_VIEW_PRELOAD },
        { FCONTROL, 'I', IDM_EDIT_INSERT },
        { FVIRTKEY, VK_DELETE, IDM_EDIT_DELETE },
        { FCONTROL, 'C', IDM_EDIT_COPY },
        { FCONTROL, 'F', IDM_FOCUS_FILTER }
    };
    return CreateAcceleratorTable(acc, (int)(sizeof(acc)/sizeof(acc[0])));
}

// ---------- App state ----------
struct Row {
    long long reg, addr;
    std::string val;
};

struct App {
    Paths P;
    Config cfg;
    Workspace ws;
    std::optional<long long> current;
    bool dirty=false;

    // UI handles
    HWND hwnd=nullptr, hCombo=nullptr, hBtnSwitch=nullptr, hBtnPreload=nullptr, hBtnOpen=nullptr, hBtnSave=nullptr, hBtnResolve=nullptr, hBtnExport=nullptr;
    HWND hList=nullptr, hEditValue=nullptr, hEditAddr=nullptr, hEditReg=nullptr, hBtnInsert=nullptr, hBtnDelete=nullptr, hStatus=nullptr;
    HWND hEditFilter=nullptr, hLog=nullptr, hProgress=nullptr, hToolTip=nullptr;
    HACCEL hAccel=nullptr;

    // Data for list + filter
    std::vector<Row> rows;           // full set
    std::vector<int>  visibleIndex;  // mapping after filter

    // Background work state
    std::atomic<bool> busy{false};

    // ------------- startup sequence -------------
    void initCore(){
        P.ensure();
        cfg = ::scripted::loadConfig(P);
        hAccel = buildAccelerators();
        logLine("Ready.");
        preloadAllUI();
    }

    void createMenu(){
        HMENU hMenubar = CreateMenu();
        HMENU hFile = CreateMenu();
        AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN,  L"&Open...\tCtrl+O");
        AppendMenuW(hFile, MF_STRING, IDM_FILE_SAVE,  L"&Save\tCtrl+S");
        AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT,  L"E&xit");
        AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hFile, L"&File");

        HMENU hEdit = CreateMenu();
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_INSERT, L"&Insert/Update\tCtrl+I");
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_DELETE, L"&Delete\tDel");
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY,   L"&Copy (TSV)\tCtrl+C");
        AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hEdit, L"&Edit");

        HMENU hView = CreateMenu();
        AppendMenuW(hView, MF_STRING, IDM_VIEW_PRELOAD, L"&Preload banks\tF5");
        AppendMenuW(hView, MF_STRING, IDM_VIEW_RELOAD,  L"&Reload current");
        AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hView, L"&View");

        HMENU hAction = CreateMenu();
        AppendMenuW(hAction, MF_STRING, IDM_ACTION_RESOLVE, L"&Resolve\tCtrl+R");
        AppendMenuW(hAction, MF_STRING, IDM_ACTION_EXPORT,  L"&Export JSON\tCtrl+E");
        AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hAction, L"&Actions");

        HMENU hHelp = CreateMenu();
        AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT,  L"&About");
        AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hHelp, L"&Help");

        SetMenu(hwnd, hMenubar);
    }

    void createTooltips(){
        hToolTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hToolTip, TTM_SETMAXTIPWIDTH, 0, 400);
    }
    void addTooltip(HWND target, const wchar_t* text){
        if (!hToolTip || !target) return;
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId  = (UINT_PTR)target;
        ti.lpszText = const_cast<wchar_t*>(text);
        GetClientRect(target, &ti.rect);
        SendMessageW(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
    }
    void attachTooltips(){
        addTooltip(hBtnSwitch,  L"Switch to the context typed above (Enter also works).");
        addTooltip(hBtnPreload, L"Load all banks from the files/ directory (F5).");
        addTooltip(hBtnOpen,    L"Open an existing bank file.");
        addTooltip(hBtnSave,    L"Save the current bank file (Ctrl+S).");
        addTooltip(hBtnResolve, L"Resolve the current bank (Ctrl+R).");
        addTooltip(hBtnExport,  L"Export current bank as JSON (Ctrl+E).");
        addTooltip(hEditFilter, L"Filter rows by register, address, or value (Ctrl+F focuses here).");
        addTooltip(hBtnInsert,  L"Insert or update at the given Reg & Addr (Enter).");
        addTooltip(hBtnDelete,  L"Delete the selected row (Del).");
    }

    // ------------- layout -------------
    void layout(){
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        int pad=8, row=28, btnW=90, btnH=24;

        int top = pad + 22; // room for menu bar
        MoveWindow(hCombo, pad, top, 240, row, TRUE);
        int x = pad + 240 + 6;
        MoveWindow(hBtnSwitch,  x, top, 80, btnH, TRUE); x += 80 + 6;
        MoveWindow(hBtnPreload, x, top, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnOpen,    x, top, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnSave,    x, top, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnResolve, x, top, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnExport,  x, top, btnW, btnH, TRUE);

        int top2 = top + row + 6;
        MoveWindow(hEditFilter, pad, top2, 240, row, TRUE);

        int listTop = top2 + row + 6;
        int listH = (H - listTop - 140);
        int listW = W/2 - (pad*1.5);
        int rightW = W - listW - pad*3;

        MoveWindow(hList, pad, listTop, listW, listH, TRUE);

        int rightX = pad*2 + listW;
        MoveWindow(hEditValue, rightX, listTop, rightW, listH - (row + 10), TRUE);

        int bottomY = listTop + listH - row;
        int editBoxW = 90;
        MoveWindow(hEditReg,  rightX, bottomY, 60, row, TRUE);
        MoveWindow(hEditAddr, rightX+60+6, bottomY, editBoxW, row, TRUE);
        MoveWindow(hBtnInsert,rightX+60+6+editBoxW+6, bottomY, 120, btnH, TRUE);
        MoveWindow(hBtnDelete,rightX+60+6+editBoxW+6+120+6, bottomY, 90, btnH, TRUE);

        MoveWindow(hProgress, pad, H - 98, W - pad*2, 16, TRUE);
        MoveWindow(hLog, pad, H - 78, W - pad*2, 50, TRUE);
        MoveWindow(hStatus, pad, H - 22, W - pad*2, 18, TRUE);
    }

    // ------------- UI creation -------------
    void createChildControls(){
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        hCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWN,
                                 0,0,0,0, hwnd, (HMENU)ID_BANK_COMBO, GetModuleHandleW(nullptr), nullptr);
        hBtnSwitch = CreateWindowExW(0, L"BUTTON", L"Switch", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                     0,0,0,0, hwnd, (HMENU)ID_BTN_SWITCH, GetModuleHandleW(nullptr), nullptr);

        hBtnPreload = CreateWindowExW(0, L"BUTTON", L"Preload", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      0,0,0,0, hwnd, (HMENU)ID_BTN_PRELOAD, GetModuleHandleW(nullptr), nullptr);
        hBtnOpen = CreateWindowExW(0, L"BUTTON", L"Open/Reload", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                   0,0,0,0, hwnd, (HMENU)ID_BTN_OPEN, GetModuleHandleW(nullptr), nullptr);
        hBtnSave = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                   0,0,0,0, hwnd, (HMENU)ID_BTN_SAVE, GetModuleHandleW(nullptr), nullptr);
        hBtnResolve = CreateWindowExW(0, L"BUTTON", L"Resolve", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      0,0,0,0, hwnd, (HMENU)ID_BTN_RESOLVE, GetModuleHandleW(nullptr), nullptr);
        hBtnExport  = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      0,0,0,0, hwnd, (HMENU)ID_BTN_EXPORT, GetModuleHandleW(nullptr), nullptr);

        hEditFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                      0,0,0,0, hwnd, (HMENU)ID_EDIT_FILTER, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hEditFilter, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter (Reg/Addr/Value)...");

        hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS,
                                0,0,0,0, hwnd, (HMENU)ID_LIST, GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        createColumns();

        hEditValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_LEFT|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
                                     0,0,0,0, hwnd, (HMENU)ID_EDIT_VALUE, GetModuleHandleW(nullptr), nullptr);
        hEditReg   = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"01", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                     0,0,0,0, hwnd, (HMENU)ID_EDIT_REG, GetModuleHandleW(nullptr), nullptr);
        hEditAddr  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                     0,0,0,0, hwnd, (HMENU)ID_EDIT_ADDR, GetModuleHandleW(nullptr), nullptr);

        hBtnInsert = CreateWindowExW(0, L"BUTTON", L"Insert/Update (Enter)", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                     0,0,0,0, hwnd, (HMENU)ID_BTN_INSERT, GetModuleHandleW(nullptr), nullptr);
        hBtnDelete = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                     0,0,0,0, hwnd, (HMENU)ID_BTN_DELETE, GetModuleHandleW(nullptr), nullptr);

        hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD|WS_VISIBLE,
                                    0,0,0,0, hwnd, (HMENU)ID_PROGRESS, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0,100));
        SendMessageW(hProgress, PBM_SETPOS, 0, 0);

        hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_READONLY,
                               0,0,0,0, hwnd, (HMENU)ID_LOG, GetModuleHandleW(nullptr), nullptr);

        hStatus = CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD|WS_VISIBLE,
                                  0,0,0,0, hwnd, (HMENU)ID_STATUS, GetModuleHandleW(nullptr), nullptr);
    }

    void createColumns(){
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
        col.pszText = (LPWSTR)L"Reg"; col.cx = 70; col.iSubItem=0; ListView_InsertColumn(hList, 0, &col);
        col.pszText = (LPWSTR)L"Addr"; col.cx = 80; col.iSubItem=1; ListView_InsertColumn(hList, 1, &col);
        col.pszText = (LPWSTR)L"Value (raw)"; col.cx = 600; col.iSubItem=2; ListView_InsertColumn(hList, 2, &col);
    }

    // ------------- status + log -------------
    void setStatus(const std::string& s){
        SetWindowTextW(hStatus, s2ws(s).c_str());
        logLine(s);
    }
    void logLine(const std::string& s){
        std::string line = "[" + nowStr() + "] " + s + "\r\n";
        int len = GetWindowTextLengthW(hLog);
        SendMessageW(hLog, EM_SETSEL, len, len);
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)s2ws(line).c_str());
    }

    // ------------- data ops -------------
    void preloadAllUI(){
        preloadAll(cfg, ws);
        setStatus("Preloaded. Total banks: " + std::to_string(ws.banks.size()));
        refreshBankCombo();
    }

    void refreshBankCombo(){
        SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
        for (auto& [id, b] : ws.banks){
            std::wstring item = s2ws(string(1,cfg.prefix)+toBaseN(id,cfg.base,cfg.widthBank) + "  (" + b.title + ")");
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)item.c_str());
        }
        if (current){
            std::wstring cur = s2ws(string(1,cfg.prefix)+toBaseN(*current,cfg.base,cfg.widthBank));
            int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
            bool found=false;
            for(int i=0;i<count;++i){
                wchar_t buf[512]; SendMessageW(hCombo, CB_GETLBTEXT, i, (LPARAM)buf);
                std::wstring w(buf);
                if (w.rfind(cur, 0)==0){ SendMessageW(hCombo, CB_SETCURSEL, i, 0); found=true; break; }
            }
            if (!found) SetWindowTextW(hCombo, cur.c_str());
        }
    }

    bool guardUnsaved(){
        if (!dirty) return true;
        int r = MessageBoxW(hwnd, L"You have unsaved changes.\nSave now?", L"Unsaved changes", MB_YESNOCANCEL|MB_ICONEXCLAMATION);
        if (r == IDCANCEL) return false;
        if (r == IDYES) saveCurrent();
        return true;
    }

	bool openCtxUI(const std::string& nameOrStem){
		std::string status;
		if (!::scripted::openCtx(cfg, ws, nameOrStem, status)) {   // read-only tolerant
			setStatus(status);
			return false;
		}

		std::string stem = nameOrStem;
		if (stem.size() > 4 && stem.ends_with(".txt")) stem.resize(stem.size() - 4);
		std::string token = (!stem.empty() && stem[0] == cfg.prefix) ? stem.substr(1) : stem;

		long long id = 0;
		parseIntBase(token, cfg.base, id);
		current = id;
		dirty   = false;

		setStatus(status);
		refreshBankCombo();
		rebuildRows();
		applyFilter();
		refreshList();
		return true;
	}



    void rebuildRows(){
        rows.clear();
        visibleIndex.clear();
        if (!current) return;
        auto& b = ws.banks[*current];
        for (auto& [rid, addrs] : b.regs){
            for (auto& [aid, val] : addrs){
                rows.push_back({rid, aid, val});
            }
        }
        visibleIndex.resize((int)rows.size());
        for (int i=0;i<(int)rows.size();++i) visibleIndex[i]=i;
    }

    void applyFilter(){
        wchar_t wbuf[256]{};
        GetWindowTextW(hEditFilter, wbuf, 255);
        std::string f = scripted::trim(ws2s(wbuf));
        std::string fLower = f; std::transform(fLower.begin(), fLower.end(), fLower.begin(), ::tolower);
        visibleIndex.clear();
        if (fLower.empty()){
            visibleIndex.resize((int)rows.size());
            for (int i=0;i<(int)rows.size();++i) visibleIndex[i]=i;
        } else {
            for (int i=0;i<(int)rows.size(); ++i){
                auto& r = rows[i];
                std::string regS = toBaseN(r.reg, cfg.base, cfg.widthReg);
                std::string addrS= toBaseN(r.addr,cfg.base, cfg.widthAddr);
                std::string valS = r.val;
                auto contains=[&](const std::string& hay){
                    std::string h = hay; std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                    return h.find(fLower)!=std::string::npos;
                };
                if (contains(regS) || contains(addrS) || contains(valS))
                    visibleIndex.push_back(i);
            }
        }
    }

    void refreshList(){
        ListView_DeleteAllItems(hList);
        for (int outIdx=0; outIdx<(int)visibleIndex.size(); ++outIdx){
            auto& r = rows[visibleIndex[outIdx]];
            LVITEMW it{}; it.mask=LVIF_TEXT; it.iItem=outIdx;
            std::wstring regW = s2ws(toBaseN(r.reg, cfg.base, cfg.widthReg));
            std::wstring addrW= s2ws(toBaseN(r.addr,cfg.base, cfg.widthAddr));
            std::wstring valW = s2ws(r.val);
            it.pszText = regW.data();
            ListView_InsertItem(hList, &it);
            ListView_SetItemText(hList, outIdx, 1, addrW.data());
            ListView_SetItemText(hList, outIdx, 2, valW.data());
        }
    }

	void saveCurrent(){
		if (!current) { setStatus("No current context"); return; }

		std::string err;
		auto path = contextFileName(cfg, *current);

		if (!::scripted::saveContextFile(cfg, path, ws.banks[*current], err)) {
			if (err.find("denied") != std::string::npos || err.find("permission") != std::string::npos)
				err += " — check folder permissions or choose a writable location.";
			setStatus("Save failed: " + err);
			MessageBoxW(hwnd, s2ws("Save failed:\n" + err).c_str(),
						L"Save error", MB_OK | MB_ICONERROR);
			return;
		}


		dirty = false;
		setStatus("Saved " + path.string());
	}


    void selectRowToEditor(){
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0) return;
        if (iSel >= (int)visibleIndex.size()) return;
        const Row& r = rows[visibleIndex[iSel]];
        SetWindowTextW(hEditReg,  s2ws(toBaseN(r.reg,  cfg.base, cfg.widthReg)).c_str());
        SetWindowTextW(hEditAddr, s2ws(toBaseN(r.addr, cfg.base, cfg.widthAddr)).c_str());
        SetWindowTextW(hEditValue,s2ws(r.val).c_str());
    }

    void insertOrUpdateFromEditor(bool viaEnter=false){
        if (!current){ setStatus("No current context"); return; }
        wchar_t regB[64]{}, addrB[64]{};
        int lenVal = GetWindowTextLengthW(hEditValue);
        std::wstring valW(lenVal, 0);
        GetWindowTextW(hEditReg, regB, 63);
        GetWindowTextW(hEditAddr, addrB, 63);
        GetWindowTextW(hEditValue, valW.data(), lenVal+1);
        std::string regS = ws2s(regB), addrS = ws2s(addrB), valS = ws2s(valW);
        if (trim(regS).empty()) regS = "1";
        if (trim(addrS).empty()){ setStatus("Address required"); return; }
        long long regId=1, addrId=0;
        if (!parseIntBase(trim(regS), cfg.base, regId)){ setStatus("Bad reg"); return; }
        if (!parseIntBase(trim(addrS), cfg.base, addrId)){ setStatus("Bad addr"); return; }

        ws.banks[*current].regs[regId][addrId] = valS; dirty=true;

        bool found=false;
        for (auto& r : rows){ if (r.reg==regId && r.addr==addrId){ r.val = valS; found=true; break; } }
        if (!found) rows.push_back({regId, addrId, valS});

        applyFilter();
        refreshList();
        setStatus(std::string(viaEnter? "Inserted (Enter): ":"Inserted/Updated: ") + toBaseN(regId,cfg.base,cfg.widthReg)+"."+toBaseN(addrId,cfg.base,cfg.widthAddr));
    }

    void deleteSelected(){
        if (!current) return;
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0) return;
        if (iSel >= (int)visibleIndex.size()) return;
        Row r = rows[visibleIndex[iSel]];
        auto& regs = ws.banks[*current].regs;
        auto itR = regs.find(r.reg);
        if (itR!=regs.end()){
            size_t n = itR->second.erase(r.addr);
            if (n>0){
                dirty=true;
                for (size_t i=0;i<rows.size();++i){
                    if (rows[i].reg==r.reg && rows[i].addr==r.addr){ rows.erase(rows.begin()+i); break; }
                }
                applyFilter(); refreshList();
                setStatus("Deleted.");
            }
        }
    }

    void copySelection(){
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0 || iSel >= (int)visibleIndex.size()) return;
        Row r = rows[visibleIndex[iSel]];
        std::string line = toBaseN(r.reg, cfg.base, cfg.widthReg) + "\t" +
                           toBaseN(r.addr,cfg.base, cfg.widthAddr) + "\t" + r.val + "\r\n";
        copyToClipboard(hwnd, line);
        setStatus("Copied selection to clipboard.");
    }

    void switchFromCombo(){
        wchar_t wbuf[512]{};
        GetWindowTextW(hCombo, wbuf, 511);
        std::string entry = trim(ws2s(wbuf));
        if (entry.empty()){ setStatus("Enter a context (e.g., x00001)"); return; }
        std::string stem = entry;
        if (stem.size()>4 && stem.substr(stem.size()-4)==".txt") stem = stem.substr(0, stem.size()-4);
        std::string token = (stem[0]==cfg.prefix)? stem.substr(1) : stem;
        long long id=0;
        if (!parseIntBase(token, cfg.base, id)){ setStatus("Bad context id: " + entry); return; }

        if (!guardUnsaved()) return;

        if (ws.banks.count(id)){
            current = id; dirty=false;
            rebuildRows(); applyFilter(); refreshList();
            setStatus("Switched to " + stem);
            refreshBankCombo();
        } else {
            openCtxUI(stem);
        }
    }

    // ------------- background resolve/export -------------
    void startResolve(){
        if (!current){ setStatus("No current context"); return; }
        if (busy.exchange(true)){ setStatus("Busy. Please wait..."); return; }
        SendMessageW(hProgress, PBM_SETPOS, 10, 0);
        setStatus("Resolving...");
        auto id=*current;
        std::thread([this,id](){
            std::string path;
            bool ok=true;
            try{
                auto txt = resolveBankToText(cfg, ws, id);
                auto outp = outResolvedName(cfg, id);
                std::ofstream out(outp, std::ios::binary); out<<txt;
                path = outp.string();
            } catch(...){
                ok=false;
            }
            PostMessageW(hwnd, WM_APP_RESOLVE_DONE, ok?1:0, (LPARAM)new std::string(path));
        }).detach();
    }

    void startExport(){
        if (!current){ setStatus("No current context"); return; }
        if (busy.exchange(true)){ setStatus("Busy. Please wait..."); return; }
        SendMessageW(hProgress, PBM_SETPOS, 10, 0);
        setStatus("Exporting JSON...");
        auto id=*current;
        std::thread([this,id](){
            std::string path;
            bool ok=true;
            try{
                auto js = exportBankToJSON(cfg, ws, id);
                auto outp = outJsonName(cfg, id);
                std::ofstream out(outp, std::ios::binary); out<<js;
                path = outp.string();
            } catch(...){
                ok=false;
            }
            PostMessageW(hwnd, WM_APP_EXPORT_DONE, ok?1:0, (LPARAM)new std::string(path));
        }).detach();
    }
};

// ---------- globals ----------
static App* gApp=nullptr;

// ---------- UI helpers ----------
static void AutoSizeColumns(HWND hList){
    ListView_SetColumnWidth(hList, 0, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(hList, 1, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(hList, 2, LVSCW_AUTOSIZE_USEHEADER);
}

// ---------- Dialogs ----------
static void DoOpenDialog(App& app){
    OPENFILENAMEW ofn{}; wchar_t buf[1024]=L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFilter = L"Bank files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf; ofn.nMaxFile = 1024;
    ofn.lpstrInitialDir = s2ws(app.P.root.string()).c_str();
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)){
        std::wstring ws(buf); std::string path = ws2s(ws);
        fs::path p(path);
        if (!app.guardUnsaved()) return;
        app.openCtxUI(p.stem().string());
    }
}

static void DoAbout(HWND h){
    MessageBoxW(h,
        L"scripted-gui\n\nA file-centric, cross-referential bank editor & resolver.\n"
        L"— Shared core with CLI (scripted.exe)\n— Background resolve/export\n— Filter, log, tooltips, and shortcuts",
        L"About", MB_OK|MB_ICONINFORMATION);
}

// ---------- Window proc ----------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l){
    App& app = *gApp;
    switch (msg){
    case WM_CREATE:{
        // *** FIXED ORDER ***
        app.hwnd = h;

        // Ensure common controls exist before creating child windows
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        app.createMenu();             // menu first (affects client rect)
        app.createChildControls();    // then children (parent is valid now)
        app.createTooltips();         // create tooltip window
        app.attachTooltips();         // and attach tips to children
        app.layout();                 // size/position them
        app.initCore();               // paths/config/preload/log/accelerators
        return 0;
    }
    case WM_SIZE:
        app.layout();
        AutoSizeColumns(app.hList);
        return 0;

    case WM_CLOSE:
        if (!app.guardUnsaved()) return 0;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_NOTIFY:{
        LPNMHDR hdr = (LPNMHDR)l;
        if (hdr->idFrom == ID_LIST){
            if (hdr->code == LVN_ITEMCHANGED){
                LPNMLISTVIEW lv = (LPNMLISTVIEW)l;
                if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)){
                    app.selectRowToEditor();
                }
            } else if (hdr->code == NM_DBLCLK){
                app.selectRowToEditor();
            }
        }
        return 0;
    }

    case WM_COMMAND:{
        int id = LOWORD(w);
        int code = HIWORD(w);

        if (id == ID_BANK_COMBO && code == CBN_SELCHANGE){
            int idx = (int)SendMessageW(app.hCombo, CB_GETCURSEL, 0, 0);
            if (idx>=0){
                if (!app.guardUnsaved()) return 0;
                wchar_t buf[512]; SendMessageW(app.hCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
                std::string line = ws2s(buf);
                std::string name = line.substr(0, line.find(' '));
                std::string token = (name[0]==app.cfg.prefix)? name.substr(1) : name;
                long long idv=0;
                if (parseIntBase(trim(std::move(token)), app.cfg.base, idv)){
                    app.current = idv;
                    app.dirty = false;
                    app.rebuildRows(); app.applyFilter(); app.refreshList();
                    app.setStatus("Switched to " + name);
                    SetWindowTextW(app.hCombo, s2ws(name).c_str());
                }
            }
            return 0;
        }

        // Buttons & menu items
        switch (id){
        case ID_BTN_SWITCH: app.switchFromCombo(); return 0;
        case ID_BTN_PRELOAD:
        case IDM_VIEW_PRELOAD: app.preloadAllUI(); return 0;
        case ID_BTN_OPEN:
        case IDM_FILE_OPEN: DoOpenDialog(app); return 0;
        case ID_BTN_SAVE:
        case IDM_FILE_SAVE: app.saveCurrent(); return 0;
        case ID_BTN_RESOLVE:
        case IDM_ACTION_RESOLVE: app.startResolve(); return 0;
        case ID_BTN_EXPORT:
        case IDM_ACTION_EXPORT: app.startExport(); return 0;
        case ID_BTN_INSERT:
        case IDM_EDIT_INSERT: app.insertOrUpdateFromEditor(false); return 0;
        case ID_BTN_DELETE:
        case IDM_EDIT_DELETE: app.deleteSelected(); return 0;
        case IDM_EDIT_COPY:   app.copySelection(); return 0;
        case IDM_VIEW_RELOAD:
            if (!app.current){ app.setStatus("No current context"); return 0; }
            if (!app.guardUnsaved()) return 0;
            app.openCtxUI(string(1,app.cfg.prefix)+toBaseN(*app.current,app.cfg.base,app.cfg.widthBank));
            return 0;
        case IDM_HELP_ABOUT: DoAbout(h); return 0;
        case IDM_FILE_EXIT: SendMessageW(h, WM_CLOSE, 0, 0); return 0;
        case IDM_FOCUS_FILTER: SetFocus(app.hEditFilter); return 0;
        }
        return 0;
    }

    case WM_KEYDOWN:{
        HWND focus = GetFocus();
        if (focus == app.hCombo && w == VK_RETURN){
            app.switchFromCombo();
            return 0;
        }
        if (focus == app.hEditValue && w == VK_RETURN){
            app.insertOrUpdateFromEditor(true);
            return 0;
        }
        return 0;
    }

    case WM_APP_RESOLVE_DONE:{
        std::unique_ptr<std::string> p((std::string*)l);
        SendMessageW(app.hProgress, PBM_SETPOS, 100, 0);
        app.busy = false;
        if (w) app.setStatus("Resolved -> " + *p);
        else   app.setStatus("Resolve failed.");
        SendMessageW(app.hProgress, PBM_SETPOS, 0, 0);
        return 0;
    }
    case WM_APP_EXPORT_DONE:{
        std::unique_ptr<std::string> p((std::string*)l);
        SendMessageW(app.hProgress, PBM_SETPOS, 100, 0);
        app.busy = false;
        if (w) app.setStatus("Exported JSON -> " + *p);
        else   app.setStatus("Export failed.");
        SendMessageW(app.hProgress, PBM_SETPOS, 0, 0);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, w, l);
}

// ---------- WinMain ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int){
    static App app; gApp=&app;

    WNDCLASSW wc{}; wc.hInstance = hInst; wc.lpszClassName = L"ScriptedGuiWnd";
    wc.lpfnWndProc = WndProc; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"scripted-gui — Bank Editor & Resolver",
                                WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
                                nullptr, nullptr, hInst, nullptr);

    MSG msg;
    HACCEL hAccel = nullptr;
    while (GetMessageW(&msg, nullptr, 0, 0)){
        // app.hAccel is created in initCore(), called during WM_CREATE.
        if (!TranslateAccelerator(hwnd, app.hAccel ? app.hAccel : hAccel, &msg)){
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
#endif // _WIN32