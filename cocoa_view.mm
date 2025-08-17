// cocoa_view.mm — AppKit View implementing IView for Presenter (macOS).
// Build:
//   clang++ -std=c++23 -fobjc-arc cocoa_view.mm -o scripted-gui-macos \
//       -framework AppKit -framework Foundation
//
// Uses your shared headers: scripted_core.hpp, frontend_contract.hpp, presenter.hpp

#if !defined(__APPLE__)
#include <cstdio>
int main(){ std::fprintf(stderr, "cocoa_view is macOS-only. Use Win32/Qt on other OS.\n"); return 1; }
#else

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <atomic>

#include "scripted_core.hpp"
#include "frontend_contract.hpp"
#include "presenter.hpp"

using namespace scripted;
using namespace scripted::ui;

// -------- Small utils --------
static inline NSString* ns(const std::string& s){ return [[NSString alloc] initWithUTF8String:s.c_str()]; }
static inline std::string cs(NSString* s){ return s ? std::string([s UTF8String]) : std::string(); }
static inline std::string nowHHMMSS(){
    NSDateFormatter* f = [NSDateFormatter new];
    [f setDateFormat:@"HH:mm:ss"];
    NSString* n = [f stringFromDate:[NSDate date]];
    return cs(n);
}

// Forward decl
struct CocoaViewImpl;

// Bridge that receives Cocoa actions & delegates back to C++ view
@interface ScriptedBridge : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSComboBoxDataSource, NSComboBoxDelegate, NSTextFieldDelegate>
@property (nonatomic, assign) CocoaViewImpl* impl;
@end

// -------- The C++ View that implements IView and owns the Cocoa widgets --------
struct CocoaViewImpl : IView {
    // formatting config (view-only)
    Paths P;
    Config cfg;

    // AppKit objects
    NSWindow*            window = nil;
    NSView*              root   = nil;

    NSComboBox*          combo  = nil;
    NSButton*            btnSwitch=nil, *btnPreload=nil, *btnOpen=nil, *btnSave=nil, *btnResolve=nil, *btnExport=nil;

    NSSearchField*       filterField = nil;

    NSTableView*         table = nil;
    NSScrollView*        tableScroll = nil;

    NSTextView*          valueText = nil;
    NSScrollView*        valueScroll = nil;

    NSTextField*         regField = nil;
    NSTextField*         addrField = nil;
    NSButton*            btnInsert = nil;
    NSButton*            btnDelete = nil;

    NSProgressIndicator* spinner = nil;
    NSTextView*          logText = nil;
    NSScrollView*        logScroll = nil;
    NSTextField*         statusText = nil;

    ScriptedBridge*      bridge = nil;

    // View state
    std::optional<long long> current;
    std::vector<std::pair<long long,std::string>> bankList;
    std::vector<Row> rows;

    CocoaViewImpl(){
        P.ensure();
        cfg = ::scripted::loadConfig(P);
        buildWindow();
        buildUI();
        wireActions();
        showStatus("Ready.");
    }

    ~CocoaViewImpl() override = default;

    // ---------- IView ----------
    void showStatus(const std::string& s) override {
        statusText.stringValue = ns(s);
        appendLog(s);
    }

    void showRows(const std::vector<Row>& rowsIn) override {
        rows = rowsIn;
        [table reloadData];
        // Resize columns to fit content
        for (NSTableColumn* c in table.tableColumns){
            [c setWidth:200.0];
            [table sizeToFit];
        }
    }

    void showCurrent(const std::optional<long long>& id) override {
        current = id;
        if (current){
            auto key = displayKey(*current);
            combo.stringValue = ns(key);
        }
    }

    void showBankList(const std::vector<std::pair<long long,std::string>>& banks) override {
        bankList = banks;
        [combo reloadData];
        if (current) combo.stringValue = ns(displayKey(*current));
    }

    void setBusy(bool on) override {
        if (on) [spinner startAnimation:nil];
        else    [spinner stopAnimation:nil];
        btnResolve.enabled = !on;
        btnExport.enabled  = !on;
    }

    void postToUi(std::function<void()> fn) override {
        auto heap = new std::function<void()>(std::move(fn));
        dispatch_async(dispatch_get_main_queue(), ^{
            std::unique_ptr<std::function<void()>> call(heap);
            (*call)();
        });
    }

    // ---------- helpers ----------
    std::string displayKey(long long id) const {
        return std::string(1, cfg.prefix) + toBaseN(id, cfg.base, cfg.widthBank);
    }

    void appendLog(const std::string& s){
        NSString* line = [NSString stringWithFormat:@"[%@] %@\n", ns(nowHHMMSS()), ns(s)];
        [[logText textStorage] appendAttributedString:[[NSAttributedString alloc] initWithString:line]];
        [logText scrollRangeToVisible:NSMakeRange(logText.string.length, 0)];
    }

    void copySelection(){
        NSInteger r = table.selectedRow;
        if (r < 0 || r >= (NSInteger)rows.size()) return;
        const auto& item = rows[(size_t)r];
        std::string tsv = toBaseN(item.reg, cfg.base, cfg.widthReg) + "\t" +
                          toBaseN(item.addr, cfg.base, cfg.widthAddr) + "\t" +
                          item.val + "\n";
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:ns(tsv) forType:NSPasteboardTypeString];
        showStatus("Copied selection to clipboard.");
    }

    void openDialog(){
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.canChooseDirectories = NO;
        p.canChooseFiles = YES;
        p.allowsMultipleSelection = NO;
        p.allowedFileTypes = @[@"txt"];
        p.directoryURL = [NSURL fileURLWithPath:ns(P.root.string())];
        if ([p runModal] == NSModalResponseOK){
            NSString* path = p.URL.path;
            std::string stem = cs(path.lastPathComponent.stringByDeletingPathExtension);
            if (onSwitch) onSwitch(stem);
        }
    }

    void switchFromCombo(){
        auto s = cs(combo.stringValue);
        if (trim(s).empty()){ showStatus("Enter a context (e.g., x00001)"); return; }
        if (onSwitch) onSwitch(s);
    }

    void insertFromEditors(){
        if (!onInsert) return;
        std::string regS  = trim(cs(regField.stringValue));
        std::string addrS = trim(cs(addrField.stringValue));
        if (regS.empty()) regS = "1";
        long long r=1, a=0;
        if (!parseIntBase(regS,  cfg.base, r)){ showStatus("Bad register"); return; }
        if (!parseIntBase(addrS, cfg.base, a)){ showStatus("Bad address");  return; }
        std::string val = cs(valueText.string);
        onInsert(r, a, val);
    }

    void deleteSelected(){
        if (!onDelete) return;
        NSInteger row = table.selectedRow;
        if (row < 0 || row >= (NSInteger)rows.size()) return;
        const auto& item = rows[(size_t)row];
        onDelete(item.reg, item.addr);
    }

    // ---------- UI construction ----------
    void buildWindow(){
        NSRect r = NSMakeRect(100, 100, 1200, 800);
        window = [[NSWindow alloc]
                  initWithContentRect:r
                  styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable)
                  backing:NSBackingStoreBuffered
                  defer:NO];
        window.title = @"scripted-gui — Bank Editor & Resolver (Cocoa)";
        [window makeKeyAndOrderFront:nil];
    }

    void buildUI(){
        root = [[NSView alloc] initWithFrame:window.contentView.bounds];
        root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        window.contentView = root;

        CGFloat pad=8, rowH=24;
        CGFloat W = root.bounds.size.width;
        CGFloat top = pad;

        // Combo
        combo = [[NSComboBox alloc] initWithFrame:NSMakeRect(pad, top, 260, rowH)];
        combo.usesDataSource = YES;
        combo.completes = YES;

        btnSwitch  = [[NSButton alloc] initWithFrame:NSMakeRect(pad+260+6, top, 80, rowH)];
        btnPreload = [[NSButton alloc] initWithFrame:NSMakeRect(pad+260+6+86, top, 90, rowH)];
        btnOpen    = [[NSButton alloc] initWithFrame:NSMakeRect(pad+260+6+86+96, top, 110, rowH)];
        btnSave    = [[NSButton alloc] initWithFrame:NSMakeRect(pad+260+6+86+96+116, top, 80, rowH)];
        btnResolve = [[NSButton alloc] initWithFrame:NSMakeRect(pad+260+6+86+96+116+86, top, 90, rowH)];
        btnExport  = [[NSButton alloc] initWithFrame:NSMakeRect(pad+260+6+86+96+116+86+96, top, 110, rowH)];

        btnSwitch.title  = @"Switch";
        btnPreload.title = @"Preload";
        btnOpen.title    = @"Open/Reload";
        btnSave.title    = @"Save";
        btnResolve.title = @"Resolve";
        btnExport.title  = @"Export JSON";
        for (NSButton* b in @[btnSwitch,btnPreload,btnOpen,btnSave,btnResolve,btnExport]){
            b.bezelStyle = NSBezelStyleRounded;
        }

        [root addSubview:combo];
        [root addSubview:btnSwitch];
        [root addSubview:btnPreload];
        [root addSubview:btnOpen];
        [root addSubview:btnSave];
        [root addSubview:btnResolve];
        [root addSubview:btnExport];

        // Filter
        top += rowH + 6;
        filterField = [[NSSearchField alloc] initWithFrame:NSMakeRect(pad, top, 260, rowH)];
        filterField.placeholderString = @"Filter (Reg/Addr/Value)...";
        [root addSubview:filterField];

        // Table + editor
        top += rowH + 6;
        CGFloat listTop = top;
        CGFloat H = root.bounds.size.height;
        CGFloat listH = (H - listTop - 140);

        CGFloat listW = W*0.5 - pad*1.5;
        CGFloat rightW = W - listW - pad*3;

        table = [[NSTableView alloc] initWithFrame:NSMakeRect(0,0,listW,listH)];
        NSTableColumn* c0 = [[NSTableColumn alloc] initWithIdentifier:@"reg"];
        c0.title = @"Reg"; c0.width=70;
        NSTableColumn* c1 = [[NSTableColumn alloc] initWithIdentifier:@"addr"];
        c1.title = @"Addr"; c1.width=80;
        NSTableColumn* c2 = [[NSTableColumn alloc] initWithIdentifier:@"val"];
        c2.title = @"Value (raw)"; c2.width=600;
        [table addTableColumn:c0];
        [table addTableColumn:c1];
        [table addTableColumn:c2];
        table.usesAlternatingRowBackgroundColors = YES;
        table.allowsMultipleSelection = NO;

        tableScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(pad, listTop, listW, listH)];
        tableScroll.documentView = table;
        tableScroll.hasVerticalScroller = YES;
        [root addSubview:tableScroll];

        // Value editor on right
        valueText = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,rightW, listH - (rowH + 10))];
        valueScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(pad*2 + listW, listTop, rightW, listH - (rowH + 10))];
        valueScroll.documentView = valueText;
        valueScroll.hasVerticalScroller = YES;
        [root addSubview:valueScroll];

        // Reg/Addr + Insert/Delete row
        CGFloat bottomY = listTop + listH - rowH;
        regField  = [[NSTextField alloc] initWithFrame:NSMakeRect(pad*2 + listW, bottomY, 60, rowH)];
        [regField setStringValue:@"01"];
        addrField = [[NSTextField alloc] initWithFrame:NSMakeRect(pad*2 + listW + 60 + 6, bottomY, 90, rowH)];
        btnInsert = [[NSButton alloc] initWithFrame:NSMakeRect(pad*2 + listW + 60 + 6 + 90 + 6, bottomY, 140, rowH)];
        btnDelete = [[NSButton alloc] initWithFrame:NSMakeRect(pad*2 + listW + 60 + 6 + 90 + 6 + 140 + 6, bottomY, 90, rowH)];
        btnInsert.title = @"Insert/Update (Enter)";
        btnDelete.title = @"Delete";
        btnInsert.bezelStyle = NSBezelStyleRounded;
        btnDelete.bezelStyle = NSBezelStyleRounded;
        [root addSubview:regField];
        [root addSubview:addrField];
        [root addSubview:btnInsert];
        [root addSubview:btnDelete];

        // Spinner + log + status
        spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(pad, H - 98, W - pad*2, 16)];
        spinner.indeterminate = YES;
        spinner.style = NSProgressIndicatorStyleSpinning;
        [root addSubview:spinner];

        logText = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0, W - pad*2, 50)];
        logText.editable = NO;
        logScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(pad, H - 78, W - pad*2, 50)];
        logScroll.documentView = logText;
        logScroll.hasVerticalScroller = YES;
        [root addSubview:logScroll];

        statusText = [[NSTextField alloc] initWithFrame:NSMakeRect(pad, H - 22, W - pad*2, 18)];
        statusText.editable = NO;
        statusText.bezeled = NO;
        statusText.drawsBackground = NO;
        [root addSubview:statusText];

        // autoresize masks
        tableScroll.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
        valueScroll.autoresizingMask = NSViewWidthSizable|NSViewHeightSizable;
        logScroll.autoresizingMask   = NSViewWidthSizable|NSViewMinYMargin;
        spinner.autoresizingMask     = NSViewWidthSizable|NSViewMinYMargin;
        statusText.autoresizingMask  = NSViewWidthSizable|NSViewMinYMargin;

        // menu bar
        buildMenuBar();
    }

    void buildMenuBar(){
        NSMenu* menubar = [NSMenu new];
        [NSApp setMainMenu:menubar];

        // App menu
        NSMenuItem* appItem = [NSMenuItem new];
        [menubar addItem:appItem];
        NSMenu* appMenu = [NSMenu new];
        [appItem setSubmenu:appMenu];
        [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];

        // File
        NSMenuItem* fileItem = [NSMenuItem new];
        [menubar addItem:fileItem];
        NSMenu* fileMenu = [NSMenu new];
        [fileItem setSubmenu:fileMenu];
        [fileMenu addItemWithTitle:@"Open…" action:@selector(onMenuOpen:) keyEquivalent:@"o"];
        [fileMenu addItemWithTitle:@"Save"   action:@selector(onMenuSave:) keyEquivalent:@"s"];

        // Edit
        NSMenuItem* editItem = [NSMenuItem new];
        [menubar addItem:editItem];
        NSMenu* editMenu = [NSMenu new];
        [editItem setSubmenu:editMenu];
        [editMenu addItemWithTitle:@"Insert/Update" action:@selector(onMenuInsert:) keyEquivalent:@"i"];
        [editMenu addItemWithTitle:@"Delete"        action:@selector(onMenuDelete:) keyEquivalent:@""];
        NSMenuItem* copyItem = [editMenu addItemWithTitle:@"Copy (TSV)" action:@selector(onMenuCopy:) keyEquivalent:@"c"];
        [copyItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];

        // View
        NSMenuItem* viewItem = [NSMenuItem new];
        [menubar addItem:viewItem];
        NSMenu* viewMenu = [NSMenu new];
        [viewItem setSubmenu:viewMenu];
        NSMenuItem* preloadItem = [viewMenu addItemWithTitle:@"Preload banks" action:@selector(onMenuPreload:) keyEquivalent:@""];
        (void)preloadItem;

        // Actions
        NSMenuItem* actItem = [NSMenuItem new];
        [menubar addItem:actItem];
        NSMenu* actMenu = [NSMenu new];
        [actItem setSubmenu:actMenu];
        [actMenu addItemWithTitle:@"Resolve"      action:@selector(onMenuResolve:) keyEquivalent:@"r"];
        [actMenu addItemWithTitle:@"Export JSON"  action:@selector(onMenuExport:)  keyEquivalent:@"e"];
    }

    void wireActions(){
        bridge = [ScriptedBridge new];
        bridge.impl = this;

        combo.dataSource = bridge;
        combo.delegate   = bridge;

        filterField.delegate = bridge;

        table.dataSource = bridge;
        table.delegate   = bridge;

        btnSwitch.target = bridge;  btnSwitch.action = @selector(onSwitch:);
        btnPreload.target= bridge;  btnPreload.action= @selector(onPreload:);
        btnOpen.target   = bridge;  btnOpen.action   = @selector(onOpen:);
        btnSave.target   = bridge;  btnSave.action   = @selector(onSave:);
        btnResolve.target= bridge;  btnResolve.action= @selector(onResolve:);
        btnExport.target = bridge;  btnExport.action = @selector(onExport:);

        btnInsert.target = bridge;  btnInsert.action = @selector(onInsert:);
        btnDelete.target = bridge;  btnDelete.action = @selector(onDelete:);
    }

    // menu handlers (forwarded via responder chain to bridge)
    - (void)onMenuOpen:(id)sender { (void)sender; openDialog(); }
    - (void)onMenuSave:(id)sender { (void)sender; if (onSave) onSave(); }
    - (void)onMenuInsert:(id)sender { (void)sender; insertFromEditors(); }
    - (void)onMenuDelete:(id)sender { (void)sender; deleteSelected(); }
    - (void)onMenuCopy:(id)sender { (void)sender; copySelection(); }
    - (void)onMenuPreload:(id)sender { (void)sender; if (onPreload) onPreload(); }
    - (void)onMenuResolve:(id)sender { (void)sender; if (onResolve) onResolve(); }
    - (void)onMenuExport:(id)sender { (void)sender; if (onExport)  onExport(); }
};

// -------- Bridge implementation --------
@implementation ScriptedBridge
// Buttons
- (void)onSwitch:(id)sender { (void)sender; if (self.impl) self.impl->switchFromCombo(); }
- (void)onPreload:(id)sender { (void)sender; if (self.impl && self.impl->onPreload) self.impl->onPreload(); }
- (void)onOpen:(id)sender { (void)sender; if (self.impl) self.impl->openDialog(); }
- (void)onSave:(id)sender { (void)sender; if (self.impl && self.impl->onSave) self.impl->onSave(); }
- (void)onResolve:(id)sender { (void)sender; if (self.impl && self.impl->onResolve) self.impl->onResolve(); }
- (void)onExport:(id)sender { (void)sender; if (self.impl && self.impl->onExport) self.impl->onExport(); }
- (void)onInsert:(id)sender { (void)sender; if (self.impl) self.impl->insertFromEditors(); }
- (void)onDelete:(id)sender { (void)sender; if (self.impl) self.impl->deleteSelected(); }

// Filter live changes
- (void)controlTextDidChange:(NSNotification *)obj {
    (void)obj;
    if (self.impl && self.impl->onFilter){
        auto s = cs(((NSTextField*)obj.object).stringValue);
        self.impl->onFilter(s);
    }
}

// Table double-click to load into editor
- (void)tableView:(NSTableView *)tableView didDoubleClickRow:(NSInteger)row {
    if (!self.impl) return;
    if (row < 0 || row >= (NSInteger)self.impl->rows.size()) return;
    const auto& r = self.impl->rows[(size_t)row];
    self.impl->regField.stringValue  = ns(toBaseN(r.reg,  self.impl->cfg.base, self.impl->cfg.widthReg));
    self.impl->addrField.stringValue = ns(toBaseN(r.addr, self.impl->cfg.base, self.impl->cfg.widthAddr));
    self.impl->valueText.string = ns(r.val);
}

// Data source
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    return self.impl ? (NSInteger)self.impl->rows.size() : 0;
}
- (nullable id)tableView:(NSTableView *)tableView objectValueForTableColumn:(nullable NSTableColumn *)tableColumn row:(NSInteger)row {
    if (!self.impl) return @"";
    const auto& r = self.impl->rows[(size_t)row];
    NSString* ident = tableColumn.identifier;
    if ([ident isEqualToString:@"reg"])  return ns(toBaseN(r.reg,  self.impl->cfg.base, self.impl->cfg.widthReg));
    if ([ident isEqualToString:@"addr"]) return ns(toBaseN(r.addr, self.impl->cfg.base, self.impl->cfg.widthAddr));
    return ns(r.val);
}

// Combo data source
- (NSInteger)numberOfItemsInComboBox:(NSComboBox *)aComboBox {
    return self.impl ? (NSInteger)self.impl->bankList.size() : 0;
}
- (id)comboBox:(NSComboBox *)aComboBox objectValueForItemAtIndex:(NSInteger)index {
    if (!self.impl) return @"";
    auto& it = self.impl->bankList[(size_t)index];
    std::string s = self.impl->displayKey(it.first) + "  (" + it.second + ")";
    return ns(s);
}
- (void)comboBoxSelectionDidChange:(NSNotification *)notification {
    if (!self.impl) return;
    NSInteger idx = self.impl->combo.indexOfSelectedItem;
    if (idx >= 0 && idx < (NSInteger)self.impl->bankList.size()){
        auto idv = self.impl->bankList[(size_t)idx].first;
        if (self.impl->onSwitch) self.impl->onSwitch(self.impl->displayKey(idv));
    }
}
@end

// -------- App entry --------
int main(int argc, char** argv){
    @autoreleasepool {
        [NSApplication sharedApplication];

        // Create view and window
        auto view = std::make_unique<CocoaViewImpl>();

        // Create Presenter (app logic)
        scripted::ui::Presenter presenter(*view, Paths{});

        // Show window and run
        [NSApp run];
        // view/presenter destruct on exit
    }
    return 0;
}

#endif // __APPLE__
