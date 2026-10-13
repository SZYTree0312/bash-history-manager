# Bash 历史管理器 v1.1

一个单文件的 C++ 小工具，用来**查看和管理 bash/zsh 的命令历史**。WSL 里直接用，纯终端，零依赖。

bash 的历史按 `↑` 翻的就是 `~/.bash_history`。`history -c` 只清当前会话的内存，**清不掉文件里已经落地的记录** —— 想真正删掉某条带密码、带 token 的命令，得直接改这个文件。

这个工具就是干这个的。形态照我自己那个 [PowerShell 历史管理器](https://github.com/SZYTree0312/powershell-history-manager)（GUI 版）来：**终端版 `bhm` + 图形版 `bhm-gui`**，功能完全一致，动文件前都会自动备份。

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

## 安装与运行（终端版 bhm）

**方式一：make（WSL / Linux）**

```
make
sudo make install    # 可选，装到 /usr/local/bin/bhm
```

**方式二：一条 g++**

```
g++ -std=c++17 -O2 -o bhm bash_history_manager.cpp
```

只依赖 C++ 标准库，不需要 pip / cargo / 任何第三方东西。Windows 上有 MSVC 也能编（`/EHsc` 必须带，正则的 try/catch 要靠它）：

```
cl /std:c++17 /EHsc /Fe:bhm bash_history_manager.cpp
```

## 用法（终端版）

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

## 图形版 bhm-gui

`bhm-gui` 是同一套逻辑的 FLTK 图形界面，功能与终端版一一对应：

- 列表展示全部历史（序号 / 时间 / 命令），搜索框实时过滤（支持忽略大小写、正则）
- 「删除选中」按 Ctrl/Shift 多选删除；「删除所有匹配」按当前搜索词批量删
- 「清空全部」需要输入 yes 确认；删除 / 清空 / 恢复前自动备份
- 「导出 CSV」「立即备份」「恢复备份…」「详细信息」「关于」

**Linux（WSL）编译**（需要 FLTK 1.4 开发包）：

```
sudo apt install libfltk-dev    # Debian/Ubuntu；WSL 里同样适用
make bhm-gui
./bhm-gui                       # 可加 --file 指定历史文件
```

**Windows 编译**：装 MSVC Build Tools + FLTK 1.4（用 CMake 生成 VS/NMake 工程，见 [fltk.org](https://www.fltk.org/)），然后：

```
cl /std:c++17 /EHsc /MT /Fe:bhm-gui bhm_gui.cpp /link fltk.lib fltk_images.lib fltk_jpeg.lib fltk_png.lib fltk_z.lib gdi32.lib user32.lib advapi32.lib shell32.lib comdlg32.lib ole32.lib uuid.lib gdiplus.lib comctl32.lib ws2_32.lib uxtheme.lib winspool.lib
```

`/MT` 静态链 CRT，出来的 exe 不依赖 VC++ 运行库。Windows 预编译版在 [Releases](https://github.com/SZYTree0312/bash-history-manager/releases) 直接下。

## 注意

- **删除和清空是永久生效的**，改的是硬盘上的真实文件。但每次动文件前会自动备份到 `~/.bhm/backups/`，`bhm restore <备份文件>` 能捞回来。
- **操作前先关掉所有终端窗口**。bash 退出时会拿内存里的历史覆盖写文件 —— 你这边删完，那边一退出又给你写回去了（PowerShell 那边 PSReadLine 也是这个毛病）。
- bash 把多行命令按行存进历史文件，本工具按行处理，一条一行。
- 脚本里用 `--yes` 跳过交互确认；不交互又不加 `--yes`，工具会拒绝动手（退出码 2），防止误删。
- 只读操作（list / search / export / info）随便用，不会改任何东西。

## 自测

```
bash tests/run.sh
```

造好 bash / zsh / 纯文本三种 fixture，把上面每个功能都跑一遍并断言。

## 许可

MIT（终端版 + GUI 版均为 MIT；GUI 版静态链接的 FLTK 1.4.2 图形库亦为 MIT 许可）
