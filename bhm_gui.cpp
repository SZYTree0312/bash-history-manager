// bhm_gui.cpp
// bhm-gui — bash/zsh 历史文件管理器（图形版）
// FLTK 1.4 静态链接，C++17。Linux / Windows(WSL) 均可编译运行。
// 逻辑与终端版 bhm（bash_history_manager.cpp）完全一致：同样的解析、
// 同样的备份策略（~/.bhm/backups/）、同样的 CSV 导出（带 BOM）。
// 形态参考树自己的 powershell-history-manager（GUI 版）。

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <FL/Fl.H>
#ifdef _WIN32
// FLTK 自带头文件里有 enum→uchar 的 C4244 警告，不是我们的代码，屏蔽掉
#pragma warning(disable : 4244)
#endif
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Flex.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define BHM_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define BHM_MKDIR(p) mkdir((p), 0755)
#endif

static const char* VERSION = "1.1.0";

// ---------------- 基础小工具（与 CLI 版共用） ----------------

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

static bool copyFile(const std::string& from, const std::string& to) {
    std::string data;
    if (!readFile(from, data)) return false;
    return writeFile(to, data);
}

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return (bool)f;
}

static std::string homeDir() {
#ifdef _WIN32
    const char* up = getenv("USERPROFILE");
    if (up && *up) return std::string(up);
#endif
    const char* h = getenv("HOME");
    if (h && *h) return std::string(h);
    return std::string(".");
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

static void ensureDir(const std::string& dir) {
    if (dir.empty()) return;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : dir) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    std::string acc = (dir[0] == '/' || dir[0] == '\\') ? std::string("/") : std::string();
    for (size_t i = 0; i < parts.size(); i++) {
        acc += parts[i];
        BHM_MKDIR(acc.c_str());
        acc += "/";
    }
}

static std::string backupDir() { return joinPath(homeDir(), ".bhm/backups"); }

static std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    if (p != std::string::npos) return path.substr(p + 1);
    return path;
}

#ifdef _WIN32
static struct tm* bhm_localtime(const time_t* t, struct tm* out) {
    return localtime_s(out, t) == 0 ? out : nullptr;
}
#else
static struct tm* bhm_localtime(const time_t* t, struct tm* out) {
    return localtime_r(t, out);
}
#endif

static std::string nowStamp() {
    time_t t = time(nullptr);
    struct tm tmv;
    if (!bhm_localtime(&t, &tmv)) return std::string("unknown");
    char buf[40];
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tmv);
    return std::string(buf);
}

static std::string fmtTs(long long ts) {
    if (ts <= 0) return std::string();
    time_t t = (time_t)ts;
    struct tm tmv;
    if (!bhm_localtime(&t, &tmv)) return std::string();
    char buf[40];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return std::string(buf);
}

// 动历史文件之前先备份一份，返回备份路径；失败返回空串。
static std::string makeBackup(const std::string& histPath) {
    ensureDir(backupDir());
    std::string base = baseName(histPath);
    if (base.empty()) base = "history";
    std::string dst = joinPath(backupDir(), base + "." + nowStamp());
    int n = 1;
    while (fileExists(dst)) {
        dst = joinPath(backupDir(), base + "." + nowStamp() + "." + std::to_string(++n));
    }
    if (!copyFile(histPath, dst)) return std::string();
    return dst;
}

// ---------------- 历史文件解析（与 CLI 版共用） ----------------

struct Entry {
    long long ts = 0; // 0 = 不知道时间
    std::string cmd;
};

enum class Kind { Bash, Zsh, Plain };

static std::vector<Entry> parseBash(const std::string& content) {
    std::vector<Entry> out;
    std::istringstream ss(content);
    std::string line;
    long long pending = 0;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '#' && isAllDigits(line.substr(1))) {
            pending = atoll(line.c_str() + 1);
            continue;
        }
        std::string t = trim(line);
        if (t.empty()) continue;
        out.push_back(Entry{pending, t});
        pending = 0;
    }
    return out;
}

static bool looksZshLine(const std::string& l) {
    if (l.empty() || l[0] != ':') return false;
    size_t i = 1;
    while (i < l.size() && std::isspace((unsigned char)l[i])) i++;
    size_t j = i;
    while (j < l.size() && std::isdigit((unsigned char)l[j])) j++;
    return j > i && j < l.size() && l[j] == ':';
}

static std::vector<Entry> parseZsh(const std::string& content) {
    std::vector<Entry> out;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (looksZshLine(line)) {
            size_t i = 1;
            while (i < line.size() && std::isspace((unsigned char)line[i])) i++;
            size_t j = i;
            while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
            long long ts = atoll(line.substr(i, j - i).c_str());
            size_t k = line.find(';', j);
            if (k == std::string::npos) continue;
            std::string cmd = trim(line.substr(k + 1));
            if (!cmd.empty()) out.push_back(Entry{ts, cmd});
        } else {
            std::string cmd = trim(line);
            if (!cmd.empty()) out.push_back(Entry{0, cmd});
        }
    }
    return out;
}

static Kind detectKind(const std::string& content, const std::string& path) {
    std::string base = baseName(path);
    if (base == ".zsh_history" || base == "zsh_history") return Kind::Zsh;
    size_t bashTs = 0, zshLines = 0;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        if (line[0] == '#' && isAllDigits(line.substr(1))) bashTs++;
        else if (looksZshLine(line)) zshLines++;
    }
    if (zshLines > bashTs && zshLines > 0) return Kind::Zsh;
    if (bashTs > 0) return Kind::Bash;
    return Kind::Plain;
}

static std::vector<Entry> parseHistory(const std::string& content, Kind k) {
    if (k == Kind::Zsh) return parseZsh(content);
    return parseBash(content);
}

static std::string serialize(const std::vector<Entry>& es, Kind k) {
    std::string out;
    for (const Entry& e : es) {
        if (k == Kind::Zsh && e.ts > 0) {
            out += ": " + std::to_string(e.ts) + ":0;" + e.cmd + "\n";
        } else if (k != Kind::Zsh && e.ts > 0) {
            out += "#" + std::to_string(e.ts) + "\n" + e.cmd + "\n";
        } else {
            out += e.cmd + "\n";
        }
    }
    return out;
}

// ---------------- 匹配（与 CLI 版共用） ----------------

struct Matcher {
    bool ok = true;
    bool regex = false;
    std::string err;
    std::string plain, plainLower;
    std::regex re;
};

static Matcher makeMatcher(const std::string& kw, bool ci, bool useRegex) {
    Matcher m;
    if (useRegex) {
        m.regex = true;
        try {
            auto flags = std::regex::ECMAScript;
            if (ci) flags |= std::regex::icase;
            m.re = std::regex(kw, flags);
        } catch (const std::regex_error& e) {
            m.ok = false;
            m.err = e.what();
        }
    } else {
        m.plain = kw;
        m.plainLower = toLower(kw);
    }
    return m;
}

static bool matchCmd(const Matcher& m, const std::string& cmd) {
    if (m.regex) return std::regex_search(cmd, m.re);
    if (!m.plainLower.empty() && m.plainLower != m.plain) {
        return toLower(cmd).find(m.plainLower) != std::string::npos;
    }
    return cmd.find(m.plain) != std::string::npos;
}

static std::string csvField(const std::string& s) {
    bool needQuote = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!needQuote) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += "\"\"";
        else q += c;
    }
    q += "\"";
    return q;
}

// ---------------- GUI ----------------

// askText 用的全局中转（FLTK 回调是 C 函数指针，拿不到 lambda 捕获）
static std::string g_askResult;

class MainWindow {
public:
    MainWindow(int w, int h, const char* fileArg)
        : win_(w, h, "bhm-gui — bash/zsh 历史管理器") {
        win_.begin();

        Fl_Flex* col = new Fl_Flex(0, 0, w, h);
        col->type(Fl_Flex::VERTICAL);
        col->begin();

        // 第一行：历史文件路径
        Fl_Flex* row1 = new Fl_Flex(0, 0, w, 34);
        row1->type(Fl_Flex::HORIZONTAL);
        row1->begin();
        Fl_Box* lblFile = new Fl_Box(0, 0, 80, 34, "历史文件:");
        pathInput_ = new Fl_Input(0, 0, 100, 34);
        btnBrowse_ = new Fl_Button(0, 0, 80, 34, "浏览…");
        btnReload_ = new Fl_Button(0, 0, 90, 34, "重新载入");
        row1->end();
        row1->fixed(lblFile, 80);
        row1->fixed(btnBrowse_, 80);
        row1->fixed(btnReload_, 90);

        // 第二行：搜索
        Fl_Flex* row2 = new Fl_Flex(0, 0, w, 34);
        row2->type(Fl_Flex::HORIZONTAL);
        row2->begin();
        Fl_Box* lblSearch = new Fl_Box(0, 0, 50, 34, "搜索:");
        searchInput_ = new Fl_Input(0, 0, 100, 34);
        chkCi_ = new Fl_Check_Button(0, 0, 110, 34, "忽略大小写");
        chkRe_ = new Fl_Check_Button(0, 0, 60, 34, "正则");
        btnDelMatch_ = new Fl_Button(0, 0, 120, 34, "删除所有匹配");
        row2->end();
        row2->fixed(lblSearch, 50);
        row2->fixed(chkCi_, 110);
        row2->fixed(chkRe_, 60);
        row2->fixed(btnDelMatch_, 120);

        // 中间：命令列表（占满剩余空间）
        browser_ = new Fl_Multi_Browser(0, 0, w, 100);

        // 按钮行
        Fl_Flex* rowBtn = new Fl_Flex(0, 0, w, 38);
        rowBtn->type(Fl_Flex::HORIZONTAL);
        rowBtn->begin();
        btnDelSel_ = new Fl_Button(0, 0, 90, 38, "删除选中");
        btnClear_ = new Fl_Button(0, 0, 90, 38, "清空全部");
        btnExport_ = new Fl_Button(0, 0, 90, 38, "导出 CSV");
        btnBackup_ = new Fl_Button(0, 0, 90, 38, "立即备份");
        btnRestore_ = new Fl_Button(0, 0, 100, 38, "恢复备份…");
        btnInfo_ = new Fl_Button(0, 0, 90, 38, "详细信息");
        btnAbout_ = new Fl_Button(0, 0, 70, 38, "关于");
        rowBtn->end();

        // 状态栏
        status_ = new Fl_Box(0, 0, w, 24, "");
        status_->box(FL_THIN_DOWN_BOX);
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

        col->end();
        col->fixed(row1, 34);
        col->fixed(row2, 34);
        col->fixed(rowBtn, 38);
        col->fixed(status_, 24);

        win_.end();
        win_.resizable(col);

        // 回调
        btnBrowse_->callback(cbBrowse, this);
        btnReload_->callback(cbReload, this);
        searchInput_->callback(cbSearch, this);
        searchInput_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY);
        chkCi_->callback(cbSearch, this);
        chkRe_->callback(cbSearch, this);
        btnDelMatch_->callback(cbDelMatch, this);
        btnDelSel_->callback(cbDelSel, this);
        btnClear_->callback(cbClear, this);
        btnExport_->callback(cbExport, this);
        btnBackup_->callback(cbBackup, this);
        btnRestore_->callback(cbRestore, this);
        btnInfo_->callback(cbInfo, this);
        btnAbout_->callback(cbAbout, this);
        win_.callback(cbQuit, this);

        // 初始文件：--file 参数 > $HISTFILE > ~/.bash_history > ~/.zsh_history
        std::string p = fileArg ? std::string(fileArg) : guessPath();
        pathInput_->value(p.c_str());
        if (!loadFile(p)) {
            setStatus("读不到 %s —— 可以用「浏览…」选一个历史文件", p.c_str());
        }
        refreshList();
    }

    void run() { win_.show(); Fl::run(); }

private:
    Fl_Window win_;
    Fl_Input* pathInput_;
    Fl_Button* btnBrowse_;
    Fl_Button* btnReload_;
    Fl_Input* searchInput_;
    Fl_Check_Button* chkCi_;
    Fl_Check_Button* chkRe_;
    Fl_Button* btnDelMatch_;
    Fl_Multi_Browser* browser_;
    Fl_Button* btnDelSel_;
    Fl_Button* btnClear_;
    Fl_Button* btnExport_;
    Fl_Button* btnBackup_;
    Fl_Button* btnRestore_;
    Fl_Button* btnInfo_;
    Fl_Button* btnAbout_;
    Fl_Box* status_;

    std::string histPath_;
    std::string content_;
    Kind kind_ = Kind::Plain;
    std::vector<Entry> entries_;
    std::vector<size_t> visible_; // 搜索过滤后实际显示的条目下标

    static std::string guessPath() {
        const char* hf = getenv("HISTFILE");
        if (hf && *hf) return std::string(hf);
        std::string bash = joinPath(homeDir(), ".bash_history");
        if (fileExists(bash)) return bash;
        return joinPath(homeDir(), ".zsh_history");
    }

    // ---------- 数据层 ----------

    bool loadFile(const std::string& path) {
        std::string content;
        if (!readFile(path, content)) return false;
        histPath_ = path;
        content_ = std::move(content);
        kind_ = detectKind(content_, histPath_);
        entries_ = parseHistory(content_, kind_);
        return true;
    }

    void refreshList() {
        visible_.clear();
        browser_->clear();
        std::string kw = searchInput_->value();
        bool filtering = !trim(kw).empty();
        Matcher m;
        if (filtering) {
            m = makeMatcher(kw, chkCi_->value() != 0, chkRe_->value() != 0);
            if (!m.ok) {
                setStatus("正则表达式有问题: %s", m.err.c_str());
                filtering = false;
            }
        }
        for (size_t i = 0; i < entries_.size(); i++) {
            if (filtering && !matchCmd(m, entries_[i].cmd)) continue;
            visible_.push_back(i);
            std::string line = std::to_string(i + 1);
            std::string ts = fmtTs(entries_[i].ts);
            line += "  ";
            line += ts.empty() ? std::string(16, ' ') : ts;
            line += "  ";
            line += entries_[i].cmd;
            browser_->add(line.c_str());
        }
        updateStatus();
    }

    void setStatus(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        status_->copy_label(buf);
    }

    void updateStatus() {
        const char* kindName = "纯文本";
        if (kind_ == Kind::Bash) kindName = "bash 时间戳";
        else if (kind_ == Kind::Zsh) kindName = "zsh 扩展";
        if (visible_.size() != entries_.size()) {
            setStatus("显示 %zu / %zu 条 | 格式: %s | 大小: %zu 字节 | 文件: %s",
                      visible_.size(), entries_.size(), kindName, content_.size(), histPath_.c_str());
        } else {
            setStatus("共 %zu 条 | 格式: %s | 大小: %zu 字节 | 文件: %s",
                      entries_.size(), kindName, content_.size(), histPath_.c_str());
        }
    }

    // 弹一个「输入文字确认」的模态框。输入等于 expect 返回 true，否则（含取消）false。
    static bool askText(const char* title, const char* prompt, const char* expect) {
        g_askResult.clear();
        Fl_Window* dlg = new Fl_Window(440, 160, title);
        Fl_Box* lab = new Fl_Box(16, 12, 408, 52, prompt);
        lab->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);
        Fl_Input* in = new Fl_Input(16, 70, 408, 30);
        Fl_Button* ok = new Fl_Button(258, 112, 80, 32, "确定");
        Fl_Button* cancel = new Fl_Button(344, 112, 80, 32, "取消");
        ok->callback([](Fl_Widget* w, void* data) {
            g_askResult = static_cast<Fl_Input*>(data)->value();
            static_cast<Fl_Window*>(w->window())->hide();
        }, in);
        cancel->callback([](Fl_Widget* w, void*) {
            g_askResult.clear();
            static_cast<Fl_Window*>(w->window())->hide();
        });
        dlg->end();
        dlg->set_modal();
        dlg->show();
        while (dlg->shown()) Fl::wait();
        delete dlg;
        return trim(g_askResult) == expect;
    }

    // ---------- 动作 ----------

    void doReload() {
        std::string p = pathInput_->value();
        if (!loadFile(p)) {
            fl_alert("读不到历史文件:\n%s\n\n（可以用「浏览…」选一个，或先确认路径正确）", p.c_str());
            return;
        }
        refreshList();
    }

    void doBrowse() {
        const char* p = fl_file_chooser("选择历史文件", "所有文件 (*)", pathInput_->value(), 0);
        if (p) {
            pathInput_->value(p);
            doReload();
        }
    }

    // 删除 entries_ 里下标在 idxs 中的条目；已自动备份。返回是否成功。
    bool removeEntries(const std::vector<size_t>& idxs) {
        if (idxs.empty()) return false;
        std::string bak = makeBackup(histPath_);
        if (bak.empty()) {
            fl_alert("备份失败，已放弃删除。\n请检查 %s 是否可写。", backupDir().c_str());
            return false;
        }
        std::vector<bool> drop(entries_.size(), false);
        for (size_t i : idxs) drop[i] = true;
        std::vector<Entry> keep;
        keep.reserve(entries_.size() - idxs.size());
        for (size_t i = 0; i < entries_.size(); i++) {
            if (!drop[i]) keep.push_back(entries_[i]);
        }
        if (!writeFile(histPath_, serialize(keep, kind_))) {
            fl_alert("写回失败: %s", histPath_.c_str());
            return false;
        }
        size_t left = keep.size();
        setStatus("已删除 %zu 条，剩余 %zu 条 | 备份在: %s", idxs.size(), left, bak.c_str());
        return true;
    }

    void doDelSel() {
        std::vector<size_t> idxs;
        for (int i = 1; i <= browser_->size(); i++) {
            if (browser_->selected(i)) idxs.push_back(visible_[(size_t)(i - 1)]);
        }
        if (idxs.empty()) {
            fl_message("先在列表里选中要删除的命令（Ctrl / Shift 可多选）。");
            return;
        }
        if (fl_choice("将从历史文件里永久删除选中的 %zu 条命令。\n删除前会自动备份到 %s。\n\n确定删除？",
                      "取消", "删除", nullptr, idxs.size(), backupDir().c_str()) != 1) {
            return;
        }
        if (removeEntries(idxs)) {
            loadFile(histPath_);
            refreshList();
        }
    }

    void doDelMatch() {
        std::string kw = searchInput_->value();
        if (trim(kw).empty()) {
            fl_message("先在搜索框输入要删除的匹配内容。");
            return;
        }
        Matcher m = makeMatcher(kw, chkCi_->value() != 0, chkRe_->value() != 0);
        if (!m.ok) {
            fl_alert("正则表达式有问题:\n%s", m.err.c_str());
            return;
        }
        std::vector<size_t> idxs;
        for (size_t i = 0; i < entries_.size(); i++) {
            if (matchCmd(m, entries_[i].cmd)) idxs.push_back(i);
        }
        if (idxs.empty()) {
            fl_message("没有匹配「%s」的命令，未改动任何内容。", kw.c_str());
            return;
        }
        if (fl_choice("将永久删除匹配「%s」的 %zu 条命令。\n删除前会自动备份到 %s。\n\n确定删除？",
                      "取消", "删除", nullptr, kw.c_str(), idxs.size(), backupDir().c_str()) != 1) {
            return;
        }
        if (removeEntries(idxs)) {
            loadFile(histPath_);
            refreshList();
        }
    }

    void doClear() {
        if (entries_.empty()) {
            fl_message("历史文件本来就是空的。");
            return;
        }
        size_t total = entries_.size();
        if (!askText("确认清空",
                     "将清空全部历史（会自动先备份）。\n如确定，请在下方输入 yes：", "yes")) {
            return;
        }
        std::string bak = makeBackup(histPath_);
        if (bak.empty()) {
            fl_alert("备份失败，已放弃清空。");
            return;
        }
        if (!writeFile(histPath_, "")) {
            fl_alert("写回失败: %s", histPath_.c_str());
            return;
        }
        loadFile(histPath_);
        refreshList();
        setStatus("已清空 %zu 条 | 备份在: %s", total, bak.c_str());
    }

    void doExport() {
        std::string def = "bash_history_" + nowStamp() + ".csv";
        const char* p = fl_file_chooser("导出 CSV（带 BOM，Excel 打开不乱码）", "CSV 文件 (*.csv)",
                                        def.c_str(), Fl_File_Chooser::SINGLE | Fl_File_Chooser::CREATE);
        if (!p) return;
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) {
            fl_alert("写不了: %s", p);
            return;
        }
        f << "\xEF\xBB\xBF"; // BOM
        f << "序号,时间,命令\n";
        for (size_t i = 0; i < visible_.size(); i++) {
            const Entry& e = entries_[visible_[i]];
            f << (i + 1) << "," << csvField(fmtTs(e.ts)) << "," << csvField(e.cmd) << "\n";
        }
        if (!f) {
            fl_alert("写入失败: %s", p);
            return;
        }
        setStatus("已导出 %zu 条到: %s", visible_.size(), p);
        fl_message("已导出 %zu 条到:\n%s", visible_.size(), p);
    }

    void doBackup() {
        std::string b = makeBackup(histPath_);
        if (b.empty()) {
            fl_alert("备份失败。请检查 %s 是否可写。", backupDir().c_str());
            return;
        }
        setStatus("已备份: %s", b.c_str());
        fl_message("已备份到:\n%s", b.c_str());
    }

    void doRestore() {
        const char* p = fl_file_chooser("选择要恢复的备份文件", "所有文件 (*)", backupDir().c_str(), 0);
        if (!p) return;
        std::string data;
        if (!readFile(p, data)) {
            fl_alert("读不到备份文件: %s", p);
            return;
        }
        size_t bakCount = parseHistory(data, detectKind(data, p)).size();
        if (fl_choice("将用备份 %s（%zu 条）覆盖当前历史 %s（%zu 条）。\n覆盖前会自动备份当前历史。\n\n确定恢复？",
                      "取消", "恢复", nullptr, p, bakCount, histPath_.c_str(), entries_.size()) != 1) {
            return;
        }
        std::string bak = makeBackup(histPath_);
        if (bak.empty()) {
            fl_alert("备份失败，已放弃恢复。");
            return;
        }
        if (!copyFile(p, histPath_)) {
            fl_alert("恢复失败。");
            return;
        }
        size_t total = entries_.size();
        loadFile(histPath_);
        refreshList();
        setStatus("已恢复 %zu 条 | 恢复前的历史已备份到: %s", total, bak.c_str());
    }

    void doInfo() {
        std::map<std::string, size_t> freq;
        for (const Entry& e : entries_) freq[e.cmd]++;
        long long first = 0, last = 0;
        for (const Entry& e : entries_) {
            if (e.ts <= 0) continue;
            if (first == 0 || e.ts < first) first = e.ts;
            if (e.ts > last) last = e.ts;
        }
        const char* kindName = "纯文本（每行一条）";
        if (kind_ == Kind::Bash) kindName = "bash 扩展格式（#时间戳）";
        else if (kind_ == Kind::Zsh) kindName = "zsh 扩展格式（: 时间戳:时长;命令）";
        std::string range = "无时间戳";
        if (first > 0) range = fmtTs(first) + " ~ " + fmtTs(last);
        fl_message("历史文件 : %s\n格式     : %s\n总条数   : %zu（去重后 %zu）\n文件大小 : %zu 字节\n时间范围 : %s\n备份目录 : %s",
                   histPath_.c_str(), kindName, entries_.size(), freq.size(),
                   content_.size(), range.c_str(), backupDir().c_str());
    }

    void doAbout() {
        fl_message("bhm-gui %s — bash/zsh 历史文件管理器（图形版）\n\n"
                   "功能与终端版 bhm 一致，动文件前都会自动备份到 %s。\n"
                   "历史文件默认按 $HISTFILE → ~/.bash_history → ~/.zsh_history 查找。\n\n"
                   "本程序使用 FLTK 1.4.2 图形库（MIT 许可）静态链接。\n"
                   "源码: https://github.com/SZYTree0312/bash-history-manager",
                   VERSION, backupDir().c_str());
    }

    // ---------- 静态回调 ----------

    static void cbBrowse(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doBrowse(); }
    static void cbReload(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doReload(); }
    static void cbSearch(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->refreshList(); }
    static void cbDelSel(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doDelSel(); }
    static void cbDelMatch(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doDelMatch(); }
    static void cbClear(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doClear(); }
    static void cbExport(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doExport(); }
    static void cbBackup(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doBackup(); }
    static void cbRestore(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doRestore(); }
    static void cbInfo(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doInfo(); }
    static void cbAbout(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->doAbout(); }
    static void cbQuit(Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->win_.hide(); }
};

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认 GBK 代码页，--help 等 stdout 输出会乱码；切一下（仅影响本进程）
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    // --file 路径：直接指定历史文件；--version / --help 方便脚本化验证
    std::string fileArg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--file") {
            if (i + 1 >= argc) {
                fprintf(stderr, "--file 缺参数\n");
                return 2;
            }
            fileArg = argv[++i];
        } else if (a == "-v" || a == "--version" || a == "version") {
            printf("bhm-gui %s\n", VERSION);
            return 0;
        } else if (a == "-h" || a == "--help" || a == "help") {
            printf("bhm-gui %s — bash/zsh 历史文件管理器（图形版）\n"
                   "用法: bhm-gui [--file 历史文件路径]\n", VERSION);
            return 0;
        }
    }
    Fl::scheme("gtk+");
    MainWindow w(940, 680, fileArg.empty() ? nullptr : fileArg.c_str());
    w.run();
    return 0;
}
