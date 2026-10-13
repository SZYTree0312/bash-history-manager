#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bhm-gui — bash/zsh 历史文件管理器的图形版。

Python 3 + tkinter，零第三方依赖。Linux / WSL / Windows 都能直接跑，不用编译。
解析、匹配、写回的行为与 C++ 终端版 bhm 完全一致（同一套历史文件、同一套备份目录）。

    python3 bhm_gui.py                  # 自动找 ~/.bash_history 或 ~/.zsh_history
    python3 bhm_gui.py --file /path/hist
    python3 bhm_gui.py --version

Linux 上缺 tkinter 的话：sudo apt install python3-tk
"""

import os
import re
import sys
import time
import shutil
import argparse

VERSION = "1.2.0"
TRUNC_LEN = 100

# ---------------------------------------------------------------- 工具


def trim(s):
    return s.strip(" \t\r\n")


def to_lower(s):
    return s.lower()


def is_all_digits(s):
    return len(s) > 0 and all(c.isdigit() for c in s)


def truncate_to(s, n):
    """按显示宽度截断（中日韩字符算两列，和终端对齐的习惯一致）。"""
    if len(s) <= n:
        return s
    w = 0
    out = []
    for ch in s:
        cw = 2 if ord(ch) > 0x2E7F else 1
        if w + cw > n - 3:
            break
        out.append(ch)
        w += cw
    return "".join(out) + "..."


def home_dir():
    if sys.platform == "win32":
        up = os.environ.get("USERPROFILE")
        if up:
            return up
    h = os.environ.get("HOME")
    return h if h else "."


def join_path(a, b):
    if not a:
        return b
    if a.endswith("/") or a.endswith("\\"):
        return a + b
    return a + "/" + b


def backup_dir():
    return join_path(home_dir(), ".bhm/backups")


def base_name(p):
    p = p.replace("\\", "/")
    return p.rsplit("/", 1)[-1]


def ensure_dir(d):
    if d:
        os.makedirs(d, exist_ok=True)


def now_stamp():
    return time.strftime("%Y%m%d-%H%M%S", time.localtime())


def fmt_ts(ts):
    if not ts or ts <= 0:
        return ""
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))


# 历史文件里可能有非法 UTF-8 字节，用 surrogateescape 读写对称，不炸也不丢内容。
def read_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            return f.read()
    except OSError:
        return None


def write_file(path, data):
    try:
        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(data)
        return True
    except OSError:
        return False


def file_exists(path):
    return os.path.isfile(path)


def make_backup(hist_path):
    """改历史文件之前先备份一份，返回备份路径；失败返回空串。"""
    ensure_dir(backup_dir())
    base = base_name(hist_path) or "history"
    dst = join_path(backup_dir(), base + "." + now_stamp())
    n = 1
    while os.path.exists(dst):
        n += 1
        dst = join_path(backup_dir(), "%s.%s.%d" % (base, now_stamp(), n))
    try:
        shutil.copyfile(hist_path, dst)
    except OSError:
        return ""
    return dst


# ---------------------------------------------------------------- 解析

BASH, ZSH, PLAIN = "bash", "zsh", "plain"


class Entry(object):
    __slots__ = ("ts", "cmd")

    def __init__(self, ts=0, cmd=""):
        self.ts = ts
        self.cmd = cmd

    def __eq__(self, other):
        return isinstance(other, Entry) and self.ts == other.ts and self.cmd == other.cmd

    def __repr__(self):
        return "Entry(ts=%r, cmd=%r)" % (self.ts, self.cmd)


def _lines(content):
    for line in content.split("\n"):
        if line.endswith("\r"):
            line = line[:-1]
        yield line


def parse_bash(content):
    """bash 扩展格式：'#1728000000' 是下一条命令的时间戳。"""
    out = []
    pending = 0
    for line in _lines(content):
        if line and line[0] == "#" and is_all_digits(line[1:]):
            try:
                pending = int(line[1:])
            except ValueError:
                pending = 0
            continue
        t = trim(line)
        if not t:
            continue
        out.append(Entry(pending, t))
        pending = 0
    return out


def looks_zsh_line(line):
    """zsh 扩展格式：': 1728000000:0;命令'"""
    if not line or line[0] != ":":
        return False
    i = 1
    while i < len(line) and line[i].isspace():
        i += 1
    j = i
    while j < len(line) and line[j].isdigit():
        j += 1
    return j > i and j < len(line) and line[j] == ":"


def parse_zsh(content):
    out = []
    for line in _lines(content):
        if looks_zsh_line(line):
            i = 1
            while i < len(line) and line[i].isspace():
                i += 1
            j = i
            while j < len(line) and line[j].isdigit():
                j += 1
            try:
                ts = int(line[i:j])
            except ValueError:
                ts = 0
            k = line.find(";", j)
            if k < 0:
                continue
            cmd = trim(line[k + 1:])
            if cmd:
                out.append(Entry(ts, cmd))
        else:
            cmd = trim(line)
            if cmd:
                out.append(Entry(0, cmd))
    return out


def detect_kind(content, path=""):
    """先看文件名（.zsh_history 直接按 zsh），再看内容里哪种格式的行多。"""
    base = base_name(path)
    if base in (".zsh_history", "zsh_history"):
        return ZSH
    bash_ts = 0
    zsh_lines = 0
    for line in _lines(content):
        if not trim(line):
            continue
        if line[0] == "#" and is_all_digits(line[1:]):
            bash_ts += 1
        elif looks_zsh_line(line):
            zsh_lines += 1
    if zsh_lines > bash_ts and zsh_lines > 0:
        return ZSH
    if bash_ts > 0:
        return BASH
    return PLAIN


def parse_history(content, kind):
    return parse_zsh(content) if kind == ZSH else parse_bash(content)


def serialize(entries, kind):
    """写回时保持原格式。"""
    out = []
    for e in entries:
        if kind == ZSH and e.ts > 0:
            out.append(": %d:0;%s\n" % (e.ts, e.cmd))
        elif kind != ZSH and e.ts > 0:
            out.append("#%d\n%s\n" % (e.ts, e.cmd))
        else:
            out.append("%s\n" % e.cmd)
    return "".join(out)


# ---------------------------------------------------------------- 匹配


class Matcher(object):
    def __init__(self, kw, ci=False, use_regex=False):
        self.ok = True
        self.err = ""
        self.kw = kw
        self.ci = ci
        self.regex = use_regex
        self.re = None
        if use_regex:
            try:
                self.re = re.compile(kw, re.IGNORECASE if ci else 0)
            except re.error as e:
                self.ok = False
                self.err = str(e)

    def match(self, cmd):
        if self.regex:
            return self.re.search(cmd) is not None
        if self.ci:
            return self.kw.lower() in cmd.lower()
        return self.kw in cmd


def match_indices(entries, matcher):
    return [i for i, e in enumerate(entries) if matcher.match(e.cmd)]


# ---------------------------------------------------------------- CSV


def csv_field(s):
    if not any(c in s for c in (",", '"', "\n", "\r")):
        return s
    return '"' + s.replace('"', '""') + '"'


def export_csv(entries, out_file):
    """带 BOM，Excel 打开不乱码。返回 (ok, 路径或错误信息)。"""
    if not out_file:
        out_file = "bash_history_%s.csv" % now_stamp()
    try:
        # utf-8-sig 会自动写 BOM
        with open(out_file, "w", encoding="utf-8-sig", errors="surrogateescape", newline="") as f:
            f.write("序号,时间,命令\n")
            for i, e in enumerate(entries):
                f.write("%d,%s,%s\n" % (i + 1, csv_field(fmt_ts(e.ts)), csv_field(e.cmd)))
    except OSError as e:
        return False, str(e)
    return True, out_file


# ---------------------------------------------------------------- 路径


def resolve_hist_path(file_opt=None):
    """--file > $HISTFILE > ~/.bash_history > ~/.zsh_history"""
    if file_opt:
        return file_opt
    hf = os.environ.get("HISTFILE")
    if hf and os.path.isfile(hf):
        return hf
    bash = join_path(home_dir(), ".bash_history")
    if os.path.isfile(bash):
        return bash
    return join_path(home_dir(), ".zsh_history")


# ---------------------------------------------------------------- GUI

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, simpledialog
except ImportError:  # 只在真正要开窗口时才报错，纯逻辑测试不受影响
    tk = None


class App(object):
    def __init__(self, root, hist_path):
        self.root = root
        self.hist_path = hist_path
        self.entries = []
        self.kind = PLAIN
        self.ci_var = None
        self.regex_var = None

        root.title("bhm-gui %s — %s" % (VERSION, hist_path))
        root.geometry("900x600")

        self._build_ui()
        self.reload()

    def _build_ui(self):
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        # 顶部按钮
        bar = ttk.Frame(self.root, padding=(6, 6, 6, 2))
        bar.grid(row=0, column=0, sticky="ew")
        ttk.Button(bar, text="打开…", command=self.on_open).pack(side="left")
        ttk.Button(bar, text="刷新", command=self.reload).pack(side="left", padx=4)
        ttk.Button(bar, text="备份", command=self.on_backup).pack(side="left")
        ttk.Button(bar, text="恢复…", command=self.on_restore).pack(side="left", padx=4)
        ttk.Button(bar, text="导出 CSV…", command=self.on_export).pack(side="left")
        ttk.Button(bar, text="清空全部", command=self.on_clear).pack(side="left", padx=4)
        ttk.Button(bar, text="详细信息", command=self.on_info).pack(side="left")

        # 搜索行
        search = ttk.Frame(self.root, padding=(6, 2))
        search.grid(row=1, column=0, sticky="ew")
        ttk.Label(search, text="搜索:").pack(side="left")
        self.search_var = tk.StringVar()
        e = ttk.Entry(search, textvariable=self.search_var)
        e.pack(side="left", fill="x", expand=True, padx=4)
        e.bind("<KeyRelease>", lambda _ev: self.apply_filter())
        self.ci_var = tk.BooleanVar(value=True)
        self.regex_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(search, text="忽略大小写", variable=self.ci_var,
                        command=self.apply_filter).pack(side="left")
        ttk.Checkbutton(search, text="正则", variable=self.regex_var,
                        command=self.apply_filter).pack(side="left", padx=4)

        # 列表
        wrap = ttk.Frame(self.root, padding=(6, 2, 6, 2))
        wrap.grid(row=2, column=0, sticky="nsew")
        self.root.rowconfigure(2, weight=1)
        cols = ("no", "time", "cmd")
        self.tree = ttk.Treeview(wrap, columns=cols, show="headings", selectmode="extended")
        self.tree.heading("no", text="序号")
        self.tree.heading("time", text="时间")
        self.tree.heading("cmd", text="命令")
        self.tree.column("no", width=60, anchor="e", stretch=False)
        self.tree.column("time", width=140, anchor="center", stretch=False)
        self.tree.column("cmd", width=600, anchor="w")
        vsb = ttk.Scrollbar(wrap, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")

        # 底部操作
        act = ttk.Frame(self.root, padding=(6, 2, 6, 6))
        act.grid(row=3, column=0, sticky="ew")
        ttk.Button(act, text="删除选中", command=self.on_delete_selected).pack(side="left")
        ttk.Button(act, text="删除所有匹配", command=self.on_delete_matched).pack(side="left", padx=4)
        self.status = tk.StringVar(value="")
        ttk.Label(act, textvariable=self.status).pack(side="right")

    # ---------- 数据

    def reload(self):
        content = read_file(self.hist_path)
        if content is None:
            content = ""
            self.set_status("读不到历史文件（新建或权限问题）: %s" % self.hist_path)
        self.kind = detect_kind(content, self.hist_path)
        self.entries = parse_history(content, self.kind)
        self.apply_filter()

    def apply_filter(self):
        kw = self.search_var.get()
        for iid in self.tree.get_children():
            self.tree.delete(iid)
        idxs = range(len(self.entries))
        if kw:
            m = Matcher(kw, self.ci_var.get(), self.regex_var.get())
            if not m.ok:
                self.set_status("正则有问题: %s" % m.err)
                return
            idxs = match_indices(self.entries, m)
        for i in idxs:
            e = self.entries[i]
            self.tree.insert("", "end", iid=str(i),
                             values=(i + 1, fmt_ts(e.ts), truncate_to(e.cmd, TRUNC_LEN)))
        self.set_status("共 %d 条，当前显示 %d 条　|　%s" % (len(self.entries), len(idxs), self.hist_path))

    def set_status(self, msg):
        self.status.set(msg)

    def selected_indices(self):
        return sorted(int(i) for i in self.tree.selection())

    def matched_indices(self):
        kw = self.search_var.get()
        if not kw:
            return []
        m = Matcher(kw, self.ci_var.get(), self.regex_var.get())
        if not m.ok:
            return []
        return match_indices(self.entries, m)

    def commit(self, keep, what):
        """备份 → 写回 → 刷新。"""
        bak = make_backup(self.hist_path)
        if not bak:
            messagebox.showerror("失败", "备份失败，放弃%s" % what)
            return
        if not write_file(self.hist_path, serialize(keep, self.kind)):
            messagebox.showerror("失败", "写回失败: %s" % self.hist_path)
            return
        self.reload()
        self.set_status("已%s，剩余 %d 条，备份在 %s" % (what, len(keep), bak))

    # ---------- 动作

    def on_open(self):
        p = filedialog.askopenfilename(title="选择历史文件", initialdir=home_dir())
        if not p:
            return
        self.hist_path = p
        self.root.title("bhm-gui %s — %s" % (VERSION, p))
        self.reload()

    def on_backup(self):
        if not file_exists(self.hist_path):
            messagebox.showinfo("备份", "历史文件不存在: %s" % self.hist_path)
            return
        bak = make_backup(self.hist_path)
        if bak:
            messagebox.showinfo("备份", "已备份到:\n%s" % bak)
        else:
            messagebox.showerror("备份", "备份失败")

    def on_restore(self):
        d = backup_dir()
        ensure_dir(d)
        p = filedialog.askopenfilename(title="选择备份文件", initialdir=d)
        if not p:
            return
        data = read_file(p)
        if data is None:
            messagebox.showerror("恢复", "读不到备份文件: %s" % p)
            return
        bak_entries = parse_history(data, detect_kind(data, p))
        if not messagebox.askyesno(
                "恢复",
                "当前 %d 条；备份里有 %d 条。\n恢复会覆盖当前历史（覆盖前自动备份当前历史）。\n\n确定恢复？"
                % (len(self.entries), len(bak_entries))):
            return
        self.commit(bak_entries, "恢复")

    def on_export(self):
        if not self.entries:
            messagebox.showinfo("导出", "没有可导出的内容")
            return
        out = filedialog.asksaveasfilename(
            title="导出 CSV", defaultextension=".csv",
            initialfile="bash_history_%s.csv" % now_stamp(),
            filetypes=[("CSV 文件", "*.csv"), ("所有文件", "*.*")])
        if not out:
            return
        ok, info = export_csv(self.entries, out)
        if ok:
            messagebox.showinfo("导出", "已导出 %d 条到:\n%s" % (len(self.entries), info))
        else:
            messagebox.showerror("导出", "导出失败: %s" % info)

    def on_clear(self):
        if not self.entries:
            messagebox.showinfo("清空", "历史文件本来就是空的")
            return
        ans = simpledialog.askstring("清空", "将清空全部 %d 条历史（会自动先备份）。\n输入 yes 确认:" % len(self.entries))
        if ans is None or trim(ans) != "yes":
            return
        self.commit([], "清空")

    def on_delete_selected(self):
        idxs = self.selected_indices()
        if not idxs:
            messagebox.showinfo("删除", "没选中任何条目")
            return
        if not messagebox.askyesno("删除",
                                   "将从历史文件里永久删除选中的 %d 条命令。\n删除前会自动备份。\n\n确定删除？" % len(idxs)):
            return
        drop = set(idxs)
        keep = [e for i, e in enumerate(self.entries) if i not in drop]
        self.commit(keep, "删除")

    def on_delete_matched(self):
        idxs = self.matched_indices()
        if not idxs:
            messagebox.showinfo("删除", "没有匹配的命令")
            return
        if not messagebox.askyesno("删除",
                                   "将永久删除匹配「%s」的 %d 条命令。\n删除前会自动备份。\n\n确定删除？"
                                   % (self.search_var.get(), len(idxs))):
            return
        drop = set(idxs)
        keep = [e for i, e in enumerate(self.entries) if i not in drop]
        self.commit(keep, "删除")

    def on_info(self):
        raw = read_file(self.hist_path) or ""
        kind_name = {BASH: "bash 时间戳格式", ZSH: "zsh 扩展格式", PLAIN: "纯文本"}[self.kind]
        try:
            size = os.path.getsize(self.hist_path)
        except OSError:
            size = 0
        messagebox.showinfo(
            "详细信息",
            "文件: %s\n格式: %s\n条目数: %d\n文件大小: %d 字节\n备份目录: %s\n版本: bhm-gui %s"
            % (self.hist_path, kind_name, len(self.entries), size, backup_dir(), VERSION))


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="bhm-gui", description="bash/zsh 历史文件管理器（图形版）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="例:\n  python3 bhm_gui.py\n  python3 bhm_gui.py --file ~/.zsh_history\n")
    ap.add_argument("--file", dest="histfile", help="指定历史文件（默认自动探测）")
    ap.add_argument("--version", action="version", version="bhm-gui %s" % VERSION)
    args = ap.parse_args(argv)

    if tk is None:
        sys.stderr.write("缺少 tkinter。Linux 上装一下: sudo apt install python3-tk\n")
        return 1

    root = tk.Tk()
    App(root, resolve_hist_path(args.histfile))
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
