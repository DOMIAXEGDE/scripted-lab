// qt_view.cpp — Qt View implementing IView for the Presenter.
// Build (Qt6 example):
//   Linux/macOS:  c++ -std=c++23 -O2 qt_view.cpp -o scripted-gui `pkg-config --cflags --libs Qt6Widgets`
//				   ./scripted-gui
//   or use CMake snippet below.
//
// Notes:
// - Requires your existing headers: scripted_core.hpp, frontend_contract.hpp, presenter.hpp
// - Presenter handles background work; we just marshal UI updates via postToUi().

#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QWidget>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtGui/QStandardItemModel>
#include <QtGui/QClipboard>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtGui/QKeyEvent>
#include <QtCore/QTime>

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>

#include "scripted_core.hpp"
#include "frontend_contract.hpp"
#include "presenter.hpp"

using namespace scripted;
using namespace scripted::ui;

static QString qFromStd(const std::string& s){ return QString::fromUtf8(s.c_str()); }
static std::string qToStd(const QString& s){ QByteArray b = s.toUtf8(); return std::string(b.constData(), (size_t)b.size()); }

class QtView final : public QMainWindow, public IView {
    //Q_OBJECT
public:
    QtView(QWidget* parent=nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle("scripted-gui — Bank Editor & Resolver (Qt)");
        resize(1200, 800);

        // Only for display formatting & parsing; all logic in Presenter
        P.ensure();
        cfg = ::scripted::loadConfig(P);

        buildMenus();
        buildUi();
        statusBar()->showMessage("Ready.");
    }

    // ───────── IView (Presenter -> View) ─────────
    void showStatus(const std::string& s) override {
        statusBar()->showMessage(qFromStd(s), 5000);
        appendLog(s);
    }

    void showRows(const std::vector<Row>& rowsIn) override {
        rows = rowsIn;
        model->setRowCount(0);
        model->setColumnCount(3);
        if (model->columnCount() == 3) {
            model->setHeaderData(0, Qt::Horizontal, "Reg");
            model->setHeaderData(1, Qt::Horizontal, "Addr");
            model->setHeaderData(2, Qt::Horizontal, "Value (raw)");
        }
        model->setRowCount((int)rows.size());
        for (int i=0;i<(int)rows.size();++i){
            const Row& r = rows[i];
            model->setData(model->index(i,0), qFromStd(toBaseN(r.reg,  cfg.base, cfg.widthReg)));
            model->setData(model->index(i,1), qFromStd(toBaseN(r.addr, cfg.base, cfg.widthAddr)));
            model->setData(model->index(i,2), qFromStd(r.val));
        }
        table->resizeColumnsToContents();
    }

    void showCurrent(const std::optional<long long>& id) override {
        current = id;
        if (current){
            const auto key = displayKey(*current);
            if (combo->currentText() != qFromStd(key))
                combo->setCurrentText(qFromStd(key));
        }
    }

    void showBankList(const std::vector<std::pair<long long,std::string>>& banks) override {
        bankList = banks;
        combo->blockSignals(true);
        combo->clear();
        for (auto& [id, title] : bankList){
            combo->addItem(qFromStd(displayKey(id) + "  (" + title + ")"));
        }
        if (current) combo->setCurrentText(qFromStd(displayKey(*current)));
        combo->blockSignals(false);
    }
	
	void showExecResult(const std::string& title,
						const std::string& stdout_json,
						const std::string& stderr_text,
						int exit_code,
						const std::filesystem::path& workdir) override {
		appendLog(title + " — exit=" + std::to_string(exit_code) +
				  (workdir.empty()? "" : " — " + workdir.string()));
		outStdout->setPlainText(qFromStd(stdout_json));
		outStderr->setPlainText(qFromStd(stderr_text));
	}


    void setBusy(bool on) override {
        progress->setVisible(on);
        progress->setRange(0, on ? 0 : 1); // 0..0 => busy indicator (Qt)
        btnResolve->setEnabled(!on);
        btnExport->setEnabled(!on);
    }

    void postToUi(std::function<void()> fn) override {
        // Marshal to UI thread
        QMetaObject::invokeMethod(this, [f=std::move(fn)](){ f(); }, Qt::QueuedConnection);
    }

    // Accelerator access not needed; Qt Actions handle them internally

private:
    // ───────── UI construction ─────────
    void buildMenus(){
        auto mbar = menuBar();

        auto file = mbar->addMenu("&File");
        auto actOpen = file->addAction("&Open...\tCtrl+O");
        actOpen->setShortcut(QKeySequence::Open);
        connect(actOpen, &QAction::triggered, this, [this]{ openDialog(); });

        auto actSave = file->addAction("&Save\tCtrl+S");
        actSave->setShortcut(QKeySequence::Save);
        connect(actSave, &QAction::triggered, this, [this]{ if (onSave) onSave(); });

        file->addSeparator();
        auto actExit = file->addAction("E&xit");
        connect(actExit, &QAction::triggered, this, &QWidget::close);

        auto edit = mbar->addMenu("&Edit");
        auto actInsert = edit->addAction("&Insert/Update\tCtrl+I");
        actInsert->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
        connect(actInsert, &QAction::triggered, this, [this]{ insertFromEditors(); });

        auto actDelete = edit->addAction("&Delete\tDel");
        actDelete->setShortcut(QKeySequence::Delete);
        connect(actDelete, &QAction::triggered, this, [this]{ deleteSelected(); });

        auto actCopy = edit->addAction("&Copy (TSV)\tCtrl+C");
        actCopy->setShortcut(QKeySequence::Copy);
        connect(actCopy, &QAction::triggered, this, [this]{ copySelection(); });

        auto view = mbar->addMenu("&View");
        auto actPreload = view->addAction("&Preload banks\tF5");
        actPreload->setShortcut(QKeySequence(Qt::Key_F5));
        connect(actPreload, &QAction::triggered, this, [this]{ if (onPreload) onPreload(); });

        auto actReload = view->addAction("&Reload current");
        connect(actReload, &QAction::triggered, this, [this]{
            if (current && onSwitch) onSwitch(displayKey(*current));
        });

        auto actions = mbar->addMenu("&Actions");
        auto actResolve = actions->addAction("&Resolve\tCtrl+R");
        actResolve->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
        connect(actResolve, &QAction::triggered, this, [this]{ if (onResolve) onResolve(); });

        auto actExport = actions->addAction("&Export JSON\tCtrl+E");
        actExport->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
        connect(actExport, &QAction::triggered, this, [this]{ if (onExport) onExport(); });

        auto help = mbar->addMenu("&Help");
        auto actAbout = help->addAction("&About");
        connect(actAbout, &QAction::triggered, this, [this]{
            appendLog("scripted-gui (Qt View)\n— Presenter + Core\n— Background resolve/export\n— Filter & shortcuts");
        });
		
		auto code = mbar->addMenu("&Code");

		auto actRun = code->addAction("&Run (stdio-json)…\tCtrl+Enter");
		actRun->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
		connect(actRun, &QAction::triggered, this, [this]{ runSelected(); });

		auto actDoc = code->addAction("&Doc check");
		connect(actDoc, &QAction::triggered, this, [this]{ docCheckSelected(); });

		
    }

    void buildUi(){
        auto central = new QWidget(this);
        auto root = new QVBoxLayout(central);

        // Top row: combo + controls
        auto topRow = new QHBoxLayout();
        combo = new QComboBox(central);
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        topRow->addWidget(combo, /*stretch*/0);

        btnSwitch  = new QPushButton("Switch", central);
        btnPreload = new QPushButton("Preload", central);
        btnOpen    = new QPushButton("Open/Reload", central);
        btnSave    = new QPushButton("Save", central);
        btnResolve = new QPushButton("Resolve", central);
        btnExport  = new QPushButton("Export JSON", central);

        for (auto* b : {btnSwitch,btnPreload,btnOpen,btnSave,btnResolve,btnExport})
            topRow->addWidget(b);

        root->addLayout(topRow);

        // Filter
        auto filterRow = new QHBoxLayout();
        editFilter = new QLineEdit(central);
        editFilter->setPlaceholderText("Filter (Reg/Addr/Value)...");
        filterRow->addWidget(editFilter);
        root->addLayout(filterRow);

        // Middle: table (left) + value editor (right)
        auto midRow = new QHBoxLayout();
        table = new QTableView(central);
        model = new QStandardItemModel(table);
        table->setModel(model);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);

        midRow->addWidget(table, /*stretch*/1);

        auto right = new QVBoxLayout();
        editValue = new QPlainTextEdit(central);
        right->addWidget(editValue, /*stretch*/1);

        auto formRow = new QHBoxLayout();
        editReg  = new QLineEdit(central);
        editReg->setFixedWidth(60);
        editReg->setText("01");
        editAddr = new QLineEdit(central);
        editAddr->setFixedWidth(100);
        btnInsert = new QPushButton("Insert/Update (Enter)", central);
        btnDelete = new QPushButton("Delete", central);

        formRow->addWidget(editReg);
        formRow->addWidget(editAddr);
        formRow->addWidget(btnInsert);
        formRow->addWidget(btnDelete);
        right->addLayout(formRow);

        midRow->addLayout(right, /*stretch*/1);
        root->addLayout(midRow);

        // Progress + log
        progress = new QProgressBar(central);
        progress->setVisible(false);
        progress->setTextVisible(false);
        progress->setRange(0,1);
        root->addWidget(progress);

        log = new QPlainTextEdit(central);
        log->setReadOnly(true);
        log->setMaximumBlockCount(2000);
        root->addWidget(log);

        setCentralWidget(central);
		
		auto outRow = new QHBoxLayout();
		outStdout = new QPlainTextEdit(central);
		outStderr = new QPlainTextEdit(central);
		outStdout->setReadOnly(true);
		outStderr->setReadOnly(true);
		outStdout->setPlaceholderText("stdout.json");
		outStderr->setPlaceholderText("stderr (compiler/runtime)");
		outRow->addWidget(outStdout);
		outRow->addWidget(outStderr);
		root->addLayout(outRow);


        // ─── signals → IView callbacks ───
        connect(btnSwitch,  &QPushButton::clicked, this, [this]{ switchFromCombo(); });
        connect(combo, &QComboBox::activated, this, [this](int){
            switchFromCombo();
        });
        connect(btnPreload, &QPushButton::clicked, this, [this]{ if (onPreload) onPreload(); });
        connect(btnOpen,    &QPushButton::clicked, this, [this]{
            // "Open/Reload" behaves like Open dialog (choose) or reload current if empty
            if (!current) { openDialog(); return; }
            if (onSwitch) onSwitch(displayKey(*current));
        });
        connect(btnSave,    &QPushButton::clicked, this, [this]{ if (onSave) onSave(); });
        connect(btnResolve, &QPushButton::clicked, this, [this]{ if (onResolve) onResolve(); });
        connect(btnExport,  &QPushButton::clicked, this, [this]{ if (onExport) onExport(); });

        connect(editFilter, &QLineEdit::textChanged, this, [this](const QString& s){
            if (onFilter) onFilter(qToStd(s));
        });


        connect(btnInsert, &QPushButton::clicked, this, [this]{ insertFromEditors(); });
        connect(btnDelete, &QPushButton::clicked, this, [this]{ deleteSelected(); });
        connect(table, &QTableView::doubleClicked, this, [this](const QModelIndex& idx){
            if (!idx.isValid()) return;
            const int row = idx.row();
            if (row<0 || row >= model->rowCount()) return;
            editReg ->setText(model->index(row,0).data().toString());
            editAddr->setText(model->index(row,1).data().toString());
            editValue->setPlainText(model->index(row,2).data().toString());
        });

        // Enter in value = insert
        editValue->installEventFilter(this);
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj == editValue && ev->type() == QEvent::KeyPress){
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter){
                insertFromEditors();
                return true;
            }
        }
        return QMainWindow::eventFilter(obj, ev);
    }

    // ───────── helpers ─────────
    void appendLog(const std::string& s){
        log->appendPlainText("[" + QString::fromUtf8(nowStr().c_str()) + "] " + qFromStd(s));
    }

    static std::string nowStr(){
        // simple HH:MM:SS
        QTime t = QTime::currentTime();
        return qToStd(t.toString("HH:mm:ss"));
    }

    void openDialog(){
        const QString startDir = QString::fromStdString(P.root.string());
        const QString path = QFileDialog::getOpenFileName(this, "Open Bank",
                              startDir, "Bank files (*.txt);;All files (*.*)");
        if (path.isEmpty()) return;
        QFileInfo fi(path);
        const std::string stem = qToStd(fi.completeBaseName());
        if (onSwitch) onSwitch(stem);
    }

    void switchFromCombo(){
        const auto entry = combo->currentText().trimmed();
        if (entry.isEmpty()){ showStatus("Enter a context (e.g., x00001)"); return; }
        if (onSwitch) onSwitch(qToStd(entry));
    }

    void insertFromEditors(){
        if (!onInsert) return;
        QString regS  = editReg->text().trimmed();
        QString addrS = editAddr->text().trimmed();
        if (regS.isEmpty()) regS = "1";
        long long r=1, a=0;
        if (!parseIntBase(qToStd(regS),  cfg.base, r)){ showStatus("Bad register"); return; }
        if (!parseIntBase(qToStd(addrS), cfg.base, a)){ showStatus("Bad address");  return; }
        onInsert(r, a, qToStd(editValue->toPlainText()));
    }

    void deleteSelected(){
        if (!onDelete) return;
        auto sel = table->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        const int row = sel.first().row();
        if (row<0 || row >= model->rowCount()) return;
        const long long r = std::strtoll(qToStd(model->index(row,0).data().toString()).c_str(), nullptr, cfg.base);
        const long long a = std::strtoll(qToStd(model->index(row,1).data().toString()).c_str(), nullptr, cfg.base);
        onDelete(r,a);
    }

    void copySelection(){
        auto sel = table->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        const int row = sel.first().row();
        QString tsv = model->index(row,0).data().toString() + "\t" +
                      model->index(row,1).data().toString() + "\t" +
                      model->index(row,2).data().toString() + "\n";
        QApplication::clipboard()->setText(tsv);
        showStatus("Copied selection to clipboard.");
    }
	
	void runSelected(){
		if (!onRunCode) return;
		auto sel = table->selectionModel()->selectedRows();
		if (sel.isEmpty()){ showStatus("Select a row first."); return; }
		const int row = sel.first().row();
		long long r=0,a=0;
		if (!parseIntBase(qToStd(model->index(row,0).data().toString()), cfg.base, r) ||
			!parseIntBase(qToStd(model->index(row,1).data().toString()), cfg.base, a)) {
			showStatus("Bad reg/addr"); return;
		}
		bool ok=false;
		QString in = QInputDialog::getMultiLineText(this, "stdin.json", "JSON:", "{}", &ok);
		if (!ok) return;
		onRunCode(r, a, qToStd(in));
	}

	void docCheckSelected(){
		if (!onDocCheck) return;
		auto sel = table->selectionModel()->selectedRows();
		if (sel.isEmpty()){ showStatus("Select a row first."); return; }
		const int row = sel.first().row();
		long long r=0,a=0;
		if (!parseIntBase(qToStd(model->index(row,0).data().toString()), cfg.base, r) ||
			!parseIntBase(qToStd(model->index(row,1).data().toString()), cfg.base, a)) {
			showStatus("Bad reg/addr"); return;
		}
		onDocCheck(r, a);
	}


    std::string displayKey(long long id) const {
        return std::string(1, cfg.prefix) + toBaseN(id, cfg.base, cfg.widthBank);
    }

private:
    // View-side formatting config
    Paths P;
    Config cfg;

    // Widgets
    QComboBox* combo{};
    QPushButton *btnSwitch{}, *btnPreload{}, *btnOpen{}, *btnSave{}, *btnResolve{}, *btnExport{};
    QPushButton *btnInsert{}, *btnDelete{};   // <-- add these two
    QLineEdit *editFilter{}, *editReg{}, *editAddr{};
    QPlainTextEdit* editValue{};
    QTableView* table{};
    QStandardItemModel* model{};
    QProgressBar* progress{};
    QPlainTextEdit* log{};
	QPlainTextEdit* outStdout{};
	QPlainTextEdit* outStderr{};



    // View state
    std::optional<long long> current;
    std::vector<std::pair<long long,std::string>> bankList;
    std::vector<Row> rows;
};

// ───────── entry point (creates Presenter with the View) ─────────
int main(int argc, char** argv){
    QApplication app(argc, argv);

    auto view = std::make_unique<QtView>();
    view->show();

    // Presenter owns the logic; View is a thin shell.
    scripted::ui::Presenter presenter(*view, Paths{});

    return app.exec();
}

//#include "qt_view.moc"
