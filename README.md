# Bash 历史管理器 v1.3

用来**查看和管理 bash/zsh 的命令历史**。面向 Linux / WSL。

bash 的历史按 `↑` 翻的就是 `~/.bash_history`。`history -c` 只清当前会话的内存，**清不掉文件里已经落地的记录** —— 想真正删掉某条带密码、带 token 的命令，得直接改这个文件。

这个工具就是干这个的。形态照我自己那个 [PowerShell 历史管理器](https://github.com/SZYTree0312/powershell-history-manager)（GUI 版）来：**终端版 `bhm` + 图形版 `bhm-gui`**，功能完全一致，动文件前都会自动备份。

| 组件 | 语言 | 怎么跑 |
|---|---|---|
| `bhm`（终端版） | C++17，零依赖 | `make`（就是一条 `g++`），或 `g++ -std=c++17 -O2 -o bhm bash_history_manager.cpp` |
| `bhm-gui`（图形版） | Python 3 + tkinter | `python3 bhm_gui.py`，**免编译**，不用装任何 GUI 开发库 |

两者功能一致、逻辑同源。图形版走 Python 就是为了绕开编译链：不用装 FLTK 之类的 GUI 库，有 Python 就能跑。

## 功能

| 功能 | 说明 |
|---|---|
| 查看 | 列出全部历史，超长命令自动截断（`--full` 看全的） |
| 搜索 | 关键词过滤，`-i` 忽略大小写，`--regex` 上正则 |
| 导出 | 导出 CSV（带 BOM，Excel 打开不乱码），删之前先留一手 |
| 彻底删除 | 按模式从**原始历史文件**里永久移除匹配的命令 |
| 清空全部 | 抹掉整个历史文件 |
| 自动备份 | 每次动文件前自动备份到 `~/.bhm/backups/`，删错了能 `restore` |
| 恢复 | 用备份覆盖回历史文件 |

## 它动的是哪个文件

按优先级：`--file` 指定的 > `$HISTFILE` > `~/.bash_history` > `~/.zsh_history`。

bash 的扩展时间戳格式（`#1728000000` 行）和 zsh 的扩展格式（`: 1728000000:0;命令`）都认，写回时保持原格式。纯文本历史（每行一条）也认。

## 终端版 bhm（C++，零依赖）

```
make                            # 就是 g++ -std=c++17 -O2
sudo make install               # 可选，装到 /usr/local/bin/bhm
```

或者直接一条命令，不装 make 也行：

```
g++ -std=c++17 -O2 -o bhm bash_history_manager.cpp
./bhm info
```

只依赖 C++ 标准库，不需要 pip / cargo / 任何第三方东西。

### 用法

```
bhm list -n 50                  # 看最近 50 条
bhm search docker               # 搜历史
bhm search 'token=' -i          # 忽略大小写
bhm delete 'mysql -uroot'       # 删掉所有匹配的命令（删前自动备份，交互确认）
bhm delete 'token=' -i --regex  # 按正则删
bhm clear                       # 清空（要输入 yes）
bhm export -o backup.csv        # 导出 CSV
bhm restore ~/.bhm/backups/bash_history.20261013-225810
bhm info                        # 看历史文件状态
```

`--file` 可以指到任意历史文件，测试或管别的机器的历史都用它。

## 图形版 bhm-gui（Python，免编译）

```
python3 bhm_gui.py                      # 自动找历史文件
python3 bhm_gui.py --file ~/.zsh_history
python3 bhm_gui.py --version
```

`make gui` 一个意思。`sudo make install` 之后有 `bhm-gui` 命令，直接敲就行。

如果报 `ModuleNotFoundError: tkinter`，装系统包就行（**不是 pip 包**）：

```
sudo apt install python3-tk      # Debian / Ubuntu / WSL
sudo dnf install python3-tkinter # Fedora
```

界面：列表展示全部历史（序号 / 时间 / 命令），搜索框实时过滤（忽略大小写 / 正则），「删除选中」支持 Ctrl/Shift 多选，「删除所有匹配」按当前搜索词批量删，「清空全部」要输入 yes 确认，另有「导出 CSV」「备份」「恢复…」「详细信息」。删除 / 清空 / 恢复前同样自动备份到 `~/.bhm/backups/`。

## 注意

- **删除和清空是永久生效的**，改的是硬盘上的真实文件。但每次动文件前会自动备份到 `~/.bhm/backups/`，`bhm restore <备份文件>` 能捞回来。
- **操作前先关掉所有终端窗口**。bash 退出时会拿内存里的历史覆盖写文件 —— 你这边删完，那边一退出又给你写回去了（PowerShell 那边 PSReadLine 也是这个毛病）。
- bash 把多行命令按行存进历史文件，本工具按行处理，一条一行。
- 脚本里用 `--yes` 跳过交互确认；不交互又不加 `--yes`，工具会拒绝动手（退出码 2），防止误删。
- 只读操作（list / search / export / info）随便用，不会改任何东西。

## 自测

```
make test
```

- `tests/run.sh` —— 终端版 29 项（bash / zsh / 纯文本三种 fixture，删除 / 清空 / 恢复往返，CSV BOM，非交互拒绝）
- `tests/test_gui_core.py` —— 图形版核心逻辑 28 项（解析、格式识别、写回往返、匹配、CSV、备份）。纯函数断言，不开窗口

单独跑 Python 那套：`python3 tests/test_gui_core.py`

## 许可

MIT。图形版只用 Python 标准库 + tkinter，没有任何第三方依赖。
