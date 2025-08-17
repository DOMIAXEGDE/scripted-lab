#pragma once
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <filesystem>
#include <atomic>
#include "scripted_core.hpp"

namespace scripted::ui {

// Row to display
struct Row { long long reg{}, addr{}; std::string val; };

struct ViewModel {
    std::optional<long long> current;
    std::vector<Row> rows;         // full set
    std::string status;
    std::string filter;
    bool dirty=false;
};

// Interface that any GUI must implement
struct IView {
    virtual ~IView() = default;

    // Called by Presenter to mutate UI
    virtual void showStatus(const std::string& s) = 0;
    virtual void showRows(const std::vector<Row>& rows) = 0;
    virtual void showCurrent(const std::optional<long long>& id) = 0;
    virtual void showBankList(const std::vector<std::pair<long long,std::string>>& banks) = 0;
    virtual void setBusy(bool on) = 0;

    // Thread marshaling (Presenter can call this to run on UI thread)
    virtual void postToUi(std::function<void()> fn) = 0;

    // Wiring: the View fires these when the user acts (the Presenter subscribes)
    std::function<void(const std::string&)> onSwitch;   // e.g. "x00001" (stem or filename)
    std::function<void()>                   onPreload;
    std::function<void()>                   onSave;
    std::function<void()>                   onResolve;
    std::function<void()>                   onExport;
    std::function<void(long long,long long,const std::string&)> onInsert; // reg,addr,val
	// --- Add to struct IView (near other callbacks):
	std::function<void(long long,long long,const std::string&)> onRunCode; // reg, addr, stdin.json
	std::function<void(long long,long long)>                   onDocCheck; // reg, addr
    std::function<void(long long,long long)>                       onDelete;
    std::function<void(const std::string&)> onFilter;   // filter changed
	
	// --- Add a non-disruptive display hook (Presenter -> View):
	virtual void showExecResult(const std::string& title,
								const std::string& stdout_json,
								const std::string& stderr_text,
								int exit_code,
								const std::filesystem::path& workdir) = 0;
};

} // namespace scripted::ui
