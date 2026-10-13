// bash_history_manager.cpp
// bhm — bash/zsh 历史文件管理器
// 单文件、零依赖、C++17。WSL / Linux 终端使用；Windows + MSVC 也能编。
// 形态参考树自己的 powershell-history-manager（GUI 版），这是终端版。

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS // MSVC 嫌 getenv 不安全，ignore
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define BHM_MKDIR(p) _mkdir(p)
// _isatty 对 /dev/null（NUL 字符设备）也返回真，交互判断不能用它；
// GetConsoleMode 只在真控制台上成功。
static bool stdinIsTty() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define BHM_MKDIR(p) mkdir((p), 0755)
static bool stdinIsTty() { return isatty(0) != 0; }
#endif

static const char* VERSION = "1.1.0";
static const size_t TRUNC_LEN = 100;

// ---------------- 基础小工具 ----------------

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

static std::string truncateTo(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...";
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
        BHM_MKDIR(acc.c_str()); // 已存在就静默失败
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

// ---------------- 历史文件解析 ----------------

struct Entry {
    long long ts = 0; // 0 = 不知道时间
    std::string cmd;
};

enum class Kind { Bash, Zsh, Plain };

// bash 扩展格式："#1728000000" 是下一条命令的时间戳
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

// zsh 扩展格式：": 1728000000:0;命令"
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

// 先看文件名（.zsh_history 直接按 zsh），再看内容里哪种格式行多。
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
    return parseBash(content); // Bash 和 Plain 同一套
}

// 写回时保持原格式。
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

// ---------------- 匹配 ----------------

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
    if (!m.plainLower.empty() && m.plainLower != m.plain) { // ci 模式
        return toLower(cmd).find(m.plainLower) != std::string::npos;
    }
    return cmd.find(m.plain) != std::string::npos;
}

// ---------------- 子命令 ----------------

static void printEntry(size_t idx, const Entry& e, bool full) {
    printf("[%4zu] %-16s %s\n", idx + 1, fmtTs(e.ts).c_str(),
           full ? e.cmd.c_str() : truncateTo(e.cmd, TRUNC_LEN).c_str());
}

static int cmdList(const std::vector<Entry>& entries, bool full, long limit) {
    size_t total = entries.size();
    size_t start = 0;
    if (limit > 0 && total > (size_t)limit) start = total - (size_t)limit;
    for (size_t i = start; i < total; i++) printEntry(i, entries[i], full);
    printf("共 %zu 条\n", total);
    return 0;
}

static int cmdSearch(const std::vector<Entry>& entries, const std::string& kw, bool ci, bool useRegex) {
    if (kw.empty()) {
        fprintf(stderr, "用法: bhm search <关键词> [-i] [--regex]\n");
        return 2;
    }
    Matcher m = makeMatcher(kw, ci, useRegex);
    if (!m.ok) {
        fprintf(stderr, "正则有问题: %s\n", m.err.c_str());
        return 2;
    }
    size_t n = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (!matchCmd(m, entries[i].cmd)) continue;
        printEntry(i, entries[i], true);
        n++;
    }
    if (n == 0) printf("没有匹配「%s」的命令\n", kw.c_str());
    else printf("匹配 %zu 条\n", n);
    return 0;
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

static int cmdExport(const std::vector<Entry>& entries, const std::string& outFile) {
    std::string out = outFile.empty() ? ("bash_history_" + nowStamp() + ".csv") : outFile;
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
        fprintf(stderr, "写不了: %s\n", out.c_str());
        return 1;
    }
    f << "\xEF\xBB\xBF"; // BOM，Excel 打开不乱码
    f << "序号,时间,命令\n";
    for (size_t i = 0; i < entries.size(); i++) {
        f << (i + 1) << "," << csvField(fmtTs(entries[i].ts)) << "," << csvField(entries[i].cmd) << "\n";
    }
    if (!f) {
        fprintf(stderr, "写入失败: %s\n", out.c_str());
        return 1;
    }
    printf("已导出 %zu 条到 %s\n", entries.size(), out.c_str());
    return 0;
}

static int cmdDelete(const std::string& histPath, Kind kind, const std::vector<Entry>& entries,
                     const std::string& kw, bool ci, bool useRegex, bool yes) {
    if (kw.empty()) {
        fprintf(stderr, "用法: bhm delete <匹配模式> [-i] [--regex] [--yes]\n");
        return 2;
    }
    Matcher m = makeMatcher(kw, ci, useRegex);
    if (!m.ok) {
        fprintf(stderr, "正则有问题: %s\n", m.err.c_str());
        return 2;
    }
    std::vector<size_t> hit;
    for (size_t i = 0; i < entries.size(); i++) {
        if (matchCmd(m, entries[i].cmd)) hit.push_back(i);
    }
    if (hit.empty()) {
        printf("没有匹配「%s」的命令，未改动任何内容\n", kw.c_str());
        return 0;
    }
    printf("匹配到 %zu 条:\n", hit.size());
    size_t show = hit.size() < 20 ? hit.size() : 20;
    for (size_t k = 0; k < show; k++) printEntry(hit[k], entries[hit[k]], false);
    if (hit.size() > 20) printf("  ... 其余 %zu 条\n", hit.size() - 20);
    if (!yes) {
        if (!stdinIsTty()) {
            fprintf(stderr, "非交互终端，拒绝删除。确认请加 --yes\n");
            return 2;
        }
        printf("从历史文件里永久删掉这 %zu 条？[y/N] ", hit.size());
        std::string line;
        std::getline(std::cin, line);
        std::string t = trim(line);
        if (t != "y" && t != "Y") {
            printf("已取消，未改动任何内容\n");
            return 1;
        }
    }
    std::string bak = makeBackup(histPath);
    if (bak.empty()) {
        fprintf(stderr, "备份失败，放弃删除\n");
        return 1;
    }
    std::vector<bool> drop(entries.size(), false);
    for (size_t idx : hit) drop[idx] = true;
    std::vector<Entry> keep;
    keep.reserve(entries.size() - hit.size());
    for (size_t i = 0; i < entries.size(); i++) {
        if (!drop[i]) keep.push_back(entries[i]);
    }
    if (!writeFile(histPath, serialize(keep, kind))) {
        fprintf(stderr, "写回失败: %s\n", histPath.c_str());
        return 1;
    }
    printf("已删除 %zu 条，剩余 %zu 条\n备份在: %s\n", hit.size(), keep.size(), bak.c_str());
    return 0;
}

static int cmdClear(const std::string& histPath, const std::vector<Entry>& entries, bool yes) {
    if (entries.empty()) {
        printf("历史文件本来就是空的\n");
        return 0;
    }
    if (!yes) {
        if (!stdinIsTty()) {
            fprintf(stderr, "非交互终端，拒绝清空。确认请加 --yes\n");
            return 2;
        }
        printf("将清空全部 %zu 条历史（会自动先备份）。输入 yes 确认: ", entries.size());
        std::string line;
        std::getline(std::cin, line);
        if (trim(line) != "yes") {
            printf("已取消，未改动任何内容\n");
            return 1;
        }
    }
    std::string bak = makeBackup(histPath);
    if (bak.empty()) {
        fprintf(stderr, "备份失败，放弃清空\n");
        return 1;
    }
    if (!writeFile(histPath, "")) {
        fprintf(stderr, "写回失败: %s\n", histPath.c_str());
        return 1;
    }
    printf("已清空 %zu 条\n备份在: %s\n", entries.size(), bak.c_str());
    return 0;
}

static int cmdRestore(const std::string& histPath, const std::string& backupFile, bool yes) {
    if (backupFile.empty()) {
        fprintf(stderr, "用法: bhm restore <备份文件>\n备份目录: %s\n", backupDir().c_str());
        return 2;
    }
    std::string data;
    if (!readFile(backupFile, data)) {
        fprintf(stderr, "读不到备份文件: %s\n", backupFile.c_str());
        return 1;
    }
    std::vector<Entry> bakEntries = parseHistory(data, detectKind(data, backupFile));
    std::string cur;
    std::vector<Entry> curEntries;
    if (readFile(histPath, cur)) curEntries = parseHistory(cur, detectKind(cur, histPath));
    printf("当前 %zu 条；备份 %s 里有 %zu 条。恢复会覆盖当前历史。\n",
           curEntries.size(), backupFile.c_str(), bakEntries.size());
    if (!yes) {
        if (!stdinIsTty()) {
            fprintf(stderr, "非交互终端，拒绝恢复。确认请加 --yes\n");
            return 2;
        }
        printf("确认恢复？[y/N] ");
        std::string line;
        std::getline(std::cin, line);
        std::string t = trim(line);
        if (t != "y" && t != "Y") {
            printf("已取消\n");
            return 1;
        }
    }
    std::string bak = makeBackup(histPath); // 覆盖前也给当前历史留一份
    if (bak.empty()) {
        fprintf(stderr, "备份失败，放弃恢复\n");
        return 1;
    }
    if (!copyFile(backupFile, histPath)) {
        fprintf(stderr, "恢复失败\n");
        return 1;
    }
    printf("已恢复 %zu 条到 %s\n", bakEntries.size(), histPath.c_str());
    return 0;
}

static int cmdInfo(const std::string& histPath, const std::string& content, Kind kind,
                   const std::vector<Entry>& entries) {
    const char* kindName = "纯文本（每行一条）";
    if (kind == Kind::Bash) kindName = "bash 扩展格式（#时间戳）";
    else if (kind == Kind::Zsh) kindName = "zsh 扩展格式（: 时间戳:时长;命令）";
    std::map<std::string, size_t> freq;
    for (const Entry& e : entries) freq[e.cmd]++;
    long long first = 0, last = 0;
    for (const Entry& e : entries) {
        if (e.ts <= 0) continue;
        if (first == 0 || e.ts < first) first = e.ts;
        if (e.ts > last) last = e.ts;
    }
    printf("历史文件 : %s\n", histPath.c_str());
    printf("格式     : %s\n", kindName);
    printf("总条数   : %zu（去重后 %zu）\n", entries.size(), freq.size());
    printf("文件大小 : %zu 字节\n", content.size());
    if (first > 0) printf("时间范围 : %s ~ %s\n", fmtTs(first).c_str(), fmtTs(last).c_str());
    else printf("时间范围 : 无时间戳\n");
    return 0;
}

static void usage() {
    printf(
        "bhm %s — bash/zsh 历史文件管理器\n"
        "\n"
        "用法: bhm [--file 路径] <命令> [选项]\n"
        "\n"
        "命令:\n"
        "  list [-n N] [--full]        列出历史（超长命令默认截断到 100 字符）\n"
        "  search <关键词> [-i] [--regex]\n"
        "                                搜索历史\n"
        "  export [-o 文件.csv]         导出 CSV（带 BOM，Excel 打开不乱码）\n"
        "  delete <模式> [-i] [--regex] [--yes]\n"
        "                                从历史文件里永久删除匹配的命令\n"
        "  clear [--yes]                清空整个历史文件\n"
        "  backup                       手动备份历史文件\n"
        "  restore <备份文件> [--yes]   用备份覆盖当前历史\n"
        "  info                         看历史文件信息（路径/格式/条数）\n"
        "\n"
        "选项:\n"
        "  --file 路径    指定历史文件（默认 $HISTFILE，其次 ~/.bash_history / ~/.zsh_history）\n"
        "  -i             忽略大小写\n"
        "  --regex / -E   模式按正则走（ECMAScript 语法）\n"
        "  --yes / -y     跳过交互确认（删除/清空/恢复前都会自动备份）\n"
        "\n"
        "例子:\n"
        "  bhm list -n 50\n"
        "  bhm search docker\n"
        "  bhm delete 'token=' -i\n"
        "  bhm clear\n",
        VERSION);
}

// ---------------- 入口 ----------------

static std::string resolveHistPath(const std::string& fileOpt) {
    if (!fileOpt.empty()) return fileOpt;
    const char* hf = getenv("HISTFILE");
    if (hf && *hf) return std::string(hf);
    std::string bash = joinPath(homeDir(), ".bash_history");
    if (fileExists(bash)) return bash;
    return joinPath(homeDir(), ".zsh_history");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认 GBK 代码页，UTF-8 输出会乱码；切一下（仅影响本进程）
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 0;
    }

    std::string fileOpt;
    size_t i = 0;
    while (i < args.size()) {
        const std::string& a = args[i];
        if (a == "--file") {
            if (i + 1 >= args.size()) {
                fprintf(stderr, "--file 缺参数\n");
                return 2;
            }
            fileOpt = args[++i];
            i++;
        } else if (a == "-h" || a == "--help" || a == "help") {
            usage();
            return 0;
        } else if (a == "-v" || a == "--version" || a == "version") {
            printf("bhm %s\n", VERSION);
            return 0;
        } else {
            break;
        }
    }
    if (i >= args.size()) {
        usage();
        return 0;
    }
    std::string cmd = args[i++];

    bool ci = false, useRegex = false, yes = false, full = false;
    long limit = 0;
    std::string outFile;
    std::vector<std::string> pos;
    for (size_t k = i; k < args.size(); k++) {
        const std::string& a = args[k];
        if (a == "-i" || a == "--ignore-case") ci = true;
        else if (a == "--regex" || a == "-E") useRegex = true;
        else if (a == "--yes" || a == "-y") yes = true;
        else if (a == "--full") full = true;
        else if (a == "-o" || a == "--output") {
            if (k + 1 >= args.size()) { fprintf(stderr, "%s 缺参数\n", a.c_str()); return 2; }
            outFile = args[++k];
        } else if (a == "-n" || a == "--limit") {
            if (k + 1 >= args.size()) { fprintf(stderr, "%s 缺参数\n", a.c_str()); return 2; }
            limit = atol(args[++k].c_str());
            if (limit < 0) limit = 0;
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "未知选项: %s\n", a.c_str());
            return 2;
        } else {
            pos.push_back(a);
        }
    }
    std::string kw;
    for (size_t k = 0; k < pos.size(); k++) {
        if (k) kw += " ";
        kw += pos[k];
    }

    std::string histPath = resolveHistPath(fileOpt);
    std::string content;
    if (!readFile(histPath, content)) {
        fprintf(stderr, "读不到历史文件: %s\n", histPath.c_str());
        if (fileOpt.empty()) {
            fprintf(stderr, "（默认按 $HISTFILE → ~/.bash_history → ~/.zsh_history 找，可以用 --file 指定）\n");
        }
        return 1;
    }
    Kind kind = detectKind(content, histPath);
    std::vector<Entry> entries = parseHistory(content, kind);

    if (cmd == "list" || cmd == "ls") return cmdList(entries, full, limit);
    if (cmd == "search" || cmd == "find") return cmdSearch(entries, kw, ci, useRegex);
    if (cmd == "export") return cmdExport(entries, outFile);
    if (cmd == "delete" || cmd == "rm") return cmdDelete(histPath, kind, entries, kw, ci, useRegex, yes);
    if (cmd == "clear") return cmdClear(histPath, entries, yes);
    if (cmd == "restore") return cmdRestore(histPath, kw, yes);
    if (cmd == "info") return cmdInfo(histPath, content, kind, entries);
    if (cmd == "backup") {
        std::string b = makeBackup(histPath);
        if (b.empty()) {
            fprintf(stderr, "备份失败\n");
            return 1;
        }
        printf("已备份: %s\n", b.c_str());
        return 0;
    }

    fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    usage();
    return 2;
}
