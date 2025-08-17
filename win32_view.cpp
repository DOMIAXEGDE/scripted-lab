// win32_view.cpp — Windows View implementing IView for the Presenter.
// Build (MinGW):
// g++ -std=c++23 -O2 win32_view.cpp -o scripted-gui-cross.exe -municode -lgdi32 -lcomctl32 -lcomdlg32 -lole32 -luuid

#if !defined(_WIN32)
#  include <cstdio>
int main() {
    std::fprintf(stderr, "scripted-gui is Windows-only. Use the CLI on macOS/Linux.\n");
    return 1;
}
#else

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>

#ifdef _MSC_VER
#  pragma comment(lib, "comctl32.lib")
#endif

// Shared app pieces
#include "scripted_core.hpp"
#include "frontend_contract.hpp"
#include "presenter.hpp"

using namespace scripted;
using namespace scripted::ui;

// ────────────────────────────── helpers ──────────────────────────────
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
    char buf[32]; std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ────────────────────────────── UI IDs ───────────────────────────────
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
enum : UINT {
    WM_APP_INVOKE = WM_APP + 100
};

// ────────────────────────────── Win32View ────────────────────────────
class Win32View final : public IView {
public:
    explicit Win32View(HINSTANCE hInst)
        : hInst(hInst)
    {
        // Load config ONLY for formatting (prefix/base/widths); all logic lives in Presenter.
        P.ensure();
        cfg = ::scripted::loadConfig(P);
        registerClass();
        createWindow();
    }

    ~Win32View() override = default;

    // IView implementation (Presenter -> View)
    void showStatus(const std::string& s) override {
        SetWindowTextW(hStatus, s2ws(s).c_str());
        logLine(s);
    }
    void showRows(const std::vector<Row>& rowsIn) override {
        rows = rowsIn; // already filtered by Presenter
        ListView_DeleteAllItems(hList);
        for (int i=0;i<(int)rows.size();++i){
            const Row& r = rows[i];
            LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i;
            std::wstring regW  = s2ws(toBaseN(r.reg,  cfg.base, cfg.widthReg));
            std::wstring addrW = s2ws(toBaseN(r.addr, cfg.base, cfg.widthAddr));
            std::wstring valW  = s2ws(r.val);
            it.pszText = regW.data();
            ListView_InsertItem(hList, &it);
            ListView_SetItemText(hList, i, 1, addrW.data());
            ListView_SetItemText(hList, i, 2, valW.data());
        }
        autoSizeColumns();
    }
    void showCurrent(const std::optional<long long>& id) override {
        current = id;
        if (current){
            auto key = displayKey(*current);
            SetWindowTextW(hCombo, s2ws(key).c_str());
        }
    }
    void showBankList(const std::vector<std::pair<long long,std::string>>& banks) override {
        bankList = banks;
        SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
        for (auto& [id,title] : bankList){
            std::wstring w = s2ws(displayKey(id) + "  (" + title + ")");
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)w.c_str());
        }
        // Keep edit text in sync
        if (current) SetWindowTextW(hCombo, s2ws(displayKey(*current)).c_str());
    }
    void setBusy(bool on) override {
        SendMessageW(hProgress, PBM_SETPOS, on ? 25 : 0, 0);
        EnableWindow(hBtnResolve, !on);
        EnableWindow(hBtnExport,  !on);
    }
    void postToUi(std::function<void()> fn) override {
        auto heapFn = new std::function<void()>(std::move(fn));
        PostMessageW(hwnd, WM_APP_INVOKE, 0, (LPARAM)heapFn);
    }

    // Window loop helpers
    HACCEL accel() const { return hAccel; }
    HWND   window() const { return hwnd; }

    // Expose small helpers called from WndProc
    void onCreate(){
        createMenu();
        createChildControls();
        layout();
        buildAccelerators();
        showStatus("Ready.");
    }
    void onSize(){ layout(); autoSizeColumns(); }
    void onDestroy(){ PostQuitMessage(0); }

    void onCmdOpenDialog(){
        OPENFILENAMEW ofn{}; wchar_t buf[1024]=L"";
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Bank files (*.txt)\0*.txt\0All files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = buf; ofn.nMaxFile = 1024;
        ofn.lpstrInitialDir = s2ws(P.root.string()).c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)){
            std::wstring ws(buf); std::string path = ws2s(ws);
            fs::path p(path);
            if (onSwitch) onSwitch(p.stem().string());
        }
    }

    void onSwitchFromCombo(){
        wchar_t wbuf[512]{};
        GetWindowTextW(hCombo, wbuf, 511);
        std::string entry = ws2s(wbuf);
        if (entry.empty()){ showStatus("Enter a context (e.g., x00001)"); return; }
        if (onSwitch) onSwitch(entry);
    }

    void onInsertFromEditor(){
        if (!onInsert){ return; }
        // Parse using cfg.base (so user can type hex/decimal per config)
        wchar_t regB[64]{}, addrB[64]{};
        int lenVal = GetWindowTextLengthW(hEditValue);
        std::wstring valW(lenVal, 0);
        GetWindowTextW(hEditReg, regB, 63);
        GetWindowTextW(hEditAddr, addrB, 63);
        GetWindowTextW(hEditValue, valW.data(), lenVal+1);
        std::string regS = scripted::trim(ws2s(regB));
        std::string addrS= scripted::trim(ws2s(addrB));
        std::string valS = ws2s(valW);
        if (regS.empty()) regS = "1";
        long long r=1,a=0;
        if (!parseIntBase(regS, cfg.base, r)){ showStatus("Bad register"); return; }
        if (!parseIntBase(addrS, cfg.base, a)){ showStatus("Bad address");  return; }
        onInsert(r, a, valS);
    }

    void onDeleteSelected(){
        if (!onDelete) return;
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0 || iSel >= (int)rows.size()) return;
        onDelete(rows[iSel].reg, rows[iSel].addr);
    }

    void onListDblClk(){
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0 || iSel >= (int)rows.size()) return;
        const Row& r = rows[iSel];
        SetWindowTextW(hEditReg,  s2ws(toBaseN(r.reg,  cfg.base, cfg.widthReg)).c_str());
        SetWindowTextW(hEditAddr, s2ws(toBaseN(r.addr, cfg.base, cfg.widthAddr)).c_str());
        SetWindowTextW(hEditValue,s2ws(r.val).c_str());
    }

    void onFilterChanged(){
        wchar_t wbuf[256]{};
        GetWindowTextW(hEditFilter, wbuf, 255);
        if (onFilter) onFilter(ws2s(wbuf));
    }

private:
    // Formatting helpers (view-only convenience; logic lives in Presenter)
    std::string displayKey(long long id) const {
        return std::string(1, cfg.prefix) + toBaseN(id, cfg.base, cfg.widthBank);
    }

    void logLine(const std::string& s){
        std::string line = "["+nowStr()+"] "+s+"\r\n";
        int len = GetWindowTextLengthW(hLog);
        SendMessageW(hLog, EM_SETSEL, len, len);
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)s2ws(line).c_str());
    }

    void autoSizeColumns(){
        ListView_SetColumnWidth(hList, 0, LVSCW_AUTOSIZE_USEHEADER);
        ListView_SetColumnWidth(hList, 1, LVSCW_AUTOSIZE_USEHEADER);
        ListView_SetColumnWidth(hList, 2, LVSCW_AUTOSIZE_USEHEADER);
    }

    void registerClass(){
        WNDCLASSW wc{}; wc.hInstance = hInst; wc.lpszClassName = L"ScriptedWin32View";
        wc.lpfnWndProc = &Win32View::WndProcThunk;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
        RegisterClassW(&wc);
    }

    void createWindow(){
        hwnd = CreateWindowExW(0, L"ScriptedWin32View",
                               L"scripted-gui — Bank Editor & Resolver",
                               WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
                               nullptr, nullptr, hInst, this);
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

    void createChildControls(){
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        int pad=8, row=28, btnW=90, btnH=24;
        int top = pad + 22;

        hCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWN,
                                 pad, top, 240, row, hwnd, (HMENU)ID_BANK_COMBO, hInst, nullptr);

        int x = pad + 240 + 6;
        hBtnSwitch  = CreateWindowExW(0, L"BUTTON", L"Switch", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      x, top, 80, btnH, hwnd, (HMENU)ID_BTN_SWITCH, hInst, nullptr); x+=80+6;
        hBtnPreload = CreateWindowExW(0, L"BUTTON", L"Preload", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      x, top, btnW, btnH, hwnd, (HMENU)ID_BTN_PRELOAD, hInst, nullptr); x+=btnW+4;
        hBtnOpen    = CreateWindowExW(0, L"BUTTON", L"Open/Reload", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      x, top, btnW, btnH, hwnd, (HMENU)ID_BTN_OPEN, hInst, nullptr); x+=btnW+4;
        hBtnSave    = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      x, top, btnW, btnH, hwnd, (HMENU)ID_BTN_SAVE, hInst, nullptr); x+=btnW+4;
        hBtnResolve = CreateWindowExW(0, L"BUTTON", L"Resolve", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      x, top, btnW, btnH, hwnd, (HMENU)ID_BTN_RESOLVE, hInst, nullptr); x+=btnW+4;
        hBtnExport  = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                      x, top, btnW, btnH, hwnd, (HMENU)ID_BTN_EXPORT, hInst, nullptr);

        int top2 = top + row + 6;
        hEditFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                      pad, top2, 240, row, hwnd, (HMENU)ID_EDIT_FILTER, hInst, nullptr);
        SendMessageW(hEditFilter, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter (Reg/Addr/Value)...");

        int listTop = top2 + row + 6;
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        int listH = (H - listTop - 140);
        int listW = W/2 - (pad*1.5);
        int rightW = W - listW - pad*3;

        hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS,
                                pad, listTop, listW, listH, hwnd, (HMENU)ID_LIST, hInst, nullptr);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col{}; col.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
        col.pszText=(LPWSTR)L"Reg"; col.cx=70; col.iSubItem=0; ListView_InsertColumn(hList, 0, &col);
        col.pszText=(LPWSTR)L"Addr"; col.cx=80; col.iSubItem=1; ListView_InsertColumn(hList, 1, &col);
        col.pszText=(LPWSTR)L"Value (raw)"; col.cx=600; col.iSubItem=2; ListView_InsertColumn(hList, 2, &col);

        int rightX = pad*2 + listW;
        hEditValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_LEFT|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
                                     rightX, listTop, rightW, listH - (row + 10), hwnd, (HMENU)ID_EDIT_VALUE, hInst, nullptr);
        hEditReg   = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"01", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                     rightX, listTop + listH - row, 60, row, hwnd, (HMENU)ID_EDIT_REG, hInst, nullptr);
        hEditAddr  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                     rightX+60+6, listTop + listH - row, 90, row, hwnd, (HMENU)ID_EDIT_ADDR, hInst, nullptr);

        hBtnInsert = CreateWindowExW(0, L"BUTTON", L"Insert/Update (Enter)", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                     rightX+60+6+90+6, listTop + listH - row, 140, 24, hwnd, (HMENU)ID_BTN_INSERT, hInst, nullptr);
        hBtnDelete = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                     rightX+60+6+90+6+140+6, listTop + listH - row, 90, 24, hwnd, (HMENU)ID_BTN_DELETE, hInst, nullptr);

        hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD|WS_VISIBLE,
                                    pad, H - 98, W - pad*2, 16, hwnd, (HMENU)ID_PROGRESS, hInst, nullptr);
        SendMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0,100));
        SendMessageW(hProgress, PBM_SETPOS, 0, 0);

        hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_READONLY,
                               pad, H - 78, W - pad*2, 50, hwnd, (HMENU)ID_LOG, hInst, nullptr);

        hStatus = CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD|WS_VISIBLE,
                                  pad, H - 22, W - pad*2, 18, hwnd, (HMENU)ID_STATUS, hInst, nullptr);
    }

    void layout(){
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        int pad=8, row=28, btnW=90, btnH=24;
        int top = pad + 22;

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
        MoveWindow(hEditReg,   rightX, listTop + listH - row, 60, row, TRUE);
        MoveWindow(hEditAddr,  rightX+60+6, listTop + listH - row, 90, row, TRUE);
        MoveWindow(hBtnInsert, rightX+60+6+90+6, listTop + listH - row, 140, 24, TRUE);
        MoveWindow(hBtnDelete, rightX+60+6+90+6+140+6, listTop + listH - row, 90, 24, TRUE);

        MoveWindow(hProgress, pad, H - 98, W - pad*2, 16, TRUE);
        MoveWindow(hLog,      pad, H - 78, W - pad*2, 50, TRUE);
        MoveWindow(hStatus,   pad, H - 22, W - pad*2, 18, TRUE);
    }

    void buildAccelerators(){
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
        hAccel = CreateAcceleratorTable(acc, (int)(sizeof(acc)/sizeof(acc[0])));
    }

    void copySelectionToClipboard(){
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0 || iSel >= (int)rows.size()) return;
        const Row& r = rows[iSel];
        std::string line = toBaseN(r.reg,cfg.base,cfg.widthReg) + "\t" +
                           toBaseN(r.addr,cfg.base,cfg.widthAddr) + "\t" + r.val + "\r\n";
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, line.size()+1);
        if (hMem){
            void* p = GlobalLock(hMem);
            memcpy(p, line.c_str(), line.size()+1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
        showStatus("Copied selection to clipboard.");
    }

    // Thunk to instance WndProc
    static LRESULT CALLBACK WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l){
        Win32View* self;
        if (m == WM_NCCREATE){
            CREATESTRUCT* cs = (CREATESTRUCT*)l;
            self = (Win32View*)cs->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd = h;
        }
        self = (Win32View*)GetWindowLongPtrW(h, GWLP_USERDATA);
        return self ? self->WndProc(h,m,w,l) : DefWindowProcW(h,m,w,l);
    }

    LRESULT WndProc(HWND h, UINT msg, WPARAM w, LPARAM l){
        switch (msg){
        case WM_CREATE: onCreate(); return 0;
        case WM_SIZE:   onSize();   return 0;
        case WM_DESTROY:onDestroy();return 0;

        case WM_COMMAND:{
            int id = LOWORD(w), code = HIWORD(w);
            if (id == ID_BANK_COMBO && code == CBN_SELCHANGE){
                int idx = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                if (idx>=0 && idx<(int)bankList.size()){
                    // switch to selected
                    if (onSwitch) onSwitch(displayKey(bankList[idx].first));
                }
                return 0;
            }
			if (id == ID_EDIT_FILTER && code == EN_CHANGE) { onFilterChanged(); return 0; }
            switch (id){
            case ID_BTN_SWITCH: onSwitchFromCombo(); return 0;
            case ID_BTN_PRELOAD:
            case IDM_VIEW_PRELOAD: if (onPreload) onPreload(); return 0;
            case ID_BTN_OPEN:
            case IDM_FILE_OPEN: onCmdOpenDialog(); return 0;
            case ID_BTN_SAVE:
            case IDM_FILE_SAVE: if (onSave) onSave(); return 0;
            case ID_BTN_RESOLVE:
            case IDM_ACTION_RESOLVE: if (onResolve) onResolve(); return 0;
            case ID_BTN_EXPORT:
            case IDM_ACTION_EXPORT:  if (onExport)  onExport();  return 0;
            case ID_BTN_INSERT:
            case IDM_EDIT_INSERT: onInsertFromEditor(); return 0;
            case ID_BTN_DELETE:
            case IDM_EDIT_DELETE: onDeleteSelected(); return 0;
            case IDM_EDIT_COPY:   copySelectionToClipboard(); return 0;
            case IDM_VIEW_RELOAD:
                // just re-trigger a switch to current display key (Presenter will reload)
                if (current && onSwitch) onSwitch(displayKey(*current));
                return 0;
            case IDM_FOCUS_FILTER: SetFocus(hEditFilter); return 0;
            case IDM_FILE_EXIT: DestroyWindow(hwnd); return 0;
            case IDM_HELP_ABOUT:
                MessageBoxW(hwnd,
                    L"scripted-gui (Win32 View)\n\nUses Presenter + Core.\n"
                    L"— Resolve/Export in background\n— Filter & shortcuts\n— Cross-platform Presenter",
                    L"About", MB_OK|MB_ICONINFORMATION);
                return 0;
            }
            return 0;
        }

        case WM_KEYDOWN:{
            HWND focus = GetFocus();
            if (focus == hCombo && w == VK_RETURN){ onSwitchFromCombo(); return 0; }
            if (focus == hEditValue && w == VK_RETURN){ onInsertFromEditor(); return 0; }
            return 0;
        }

        case WM_NOTIFY:{
            LPNMHDR hdr = (LPNMHDR)l;
            if (hdr->idFrom == ID_LIST){
                if (hdr->code == NM_DBLCLK){ onListDblClk(); return 0; }
            }
            return 0;
        }

        case WM_APP_INVOKE:{
            std::unique_ptr<std::function<void()>> fn((std::function<void()>*)l);
            (*fn)();
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            // could theme filter/status if you like
            return DefWindowProcW(h,msg,w,l);
        }
        return DefWindowProcW(h,msg,w,l);
    }

private:
    // Formatting-only config (view-side)
    Paths P;
    Config cfg;

    // Win32
    HINSTANCE hInst{};
    HWND hwnd{}, hCombo{}, hBtnSwitch{}, hBtnPreload{}, hBtnOpen{}, hBtnSave{}, hBtnResolve{}, hBtnExport{};
    HWND hList{}, hEditValue{}, hEditAddr{}, hEditReg{}, hBtnInsert{}, hBtnDelete{}, hStatus{}, hEditFilter{}, hLog{}, hProgress{};
    HACCEL hAccel{};

    // View state
    std::optional<long long> current;
    std::vector<std::pair<long long,std::string>> bankList;
    std::vector<Row> rows;
};

// ────────────────────────────── entry point ──────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int){
    // Create view first (window + controls)
    auto view = std::make_unique<Win32View>(hInst);

    // Create Presenter with the view
    scripted::ui::Presenter presenter(*view, Paths{});

    // Wire filter change (live)
    // We already send filter changes via onFilter in the view when edit text changes,
    // but Win32 only notifies with EN_CHANGE; we can subclass the control or poll.
    // For simplicity, subclass: (already handled in onFilterChanged via WM_COMMAND if you add EN_CHANGE route)
    // Here we rely on the ID_EDIT_FILTER WM_COMMAND path (EN_CHANGE), but that is not included above,
    // so ensure the command handler forwards EN_CHANGE for ID_EDIT_FILTER:
    // (Alternatively, you can add: if (id==ID_EDIT_FILTER && code==EN_CHANGE) onFilterChanged(); in the WM_COMMAND switch.)

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)){
        if (!TranslateAcceleratorW(view->window(), view->accel(), &msg)){
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

#endif // _WIN32
