# bhm — bash/zsh 历史文件管理器
#
# 终端版（bhm）是 C++，Linux 上 g++ 一把就出，零依赖。
# 图形版有两条路：
#   make gui       → Python/tkinter 版，免编译，Linux/WSL/Windows 通吃（推荐）
#   make gui-fltk  → C++/FLTK 版，要装 FLTK 开发库，编出来是单个 exe

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PY ?= python3

.PHONY: all bhm gui gui-fltk test test-cli test-gui install clean help

all: bhm

bhm: bash_history_manager.cpp
	$(CXX) $(CXXFLAGS) -o bhm bash_history_manager.cpp

# 图形版默认走 Python：不用装任何 GUI 开发库
gui:
	$(PY) bhm_gui.py

# C++/FLTK 图形版（可选）
FLTK_CONFIG ?= fltk-config
gui-fltk: bhm_gui.cpp
	@command -v $(FLTK_CONFIG) >/dev/null 2>&1 || { \
	  echo "找不到 $(FLTK_CONFIG) —— FLTK 开发库没装。"; \
	  echo "  Debian/Ubuntu:  sudo apt install libfltk1.3-dev"; \
	  echo "  Fedora:         sudo dnf install fltk-devel"; \
	  echo "  不想装就用免编译的 Python 图形版: make gui"; \
	  exit 1; }
	$(CXX) $(CXXFLAGS) $$($(FLTK_CONFIG) --cxxflags) -o bhm-gui bhm_gui.cpp $$($(FLTK_CONFIG) --ldflags)

test-cli: bhm
	bash tests/run.sh

test-gui:
	$(PY) tests/test_gui_core.py

test: test-cli test-gui

install: bhm
	install -m 755 bhm /usr/local/bin/bhm
	install -m 755 bhm_gui.py /usr/local/bin/bhm-gui.py
	@echo "装好了: bhm（终端版）、bhm-gui.py（图形版，跑 python3 bhm-gui.py 或直接 bhm-gui.py）"

clean:
	rm -f bhm bhm-gui
	rm -rf __pycache__ tests/__pycache__

help:
	@echo "make         编译终端版 bhm（g++，零依赖）"
	@echo "make gui     启动 Python 图形版（免编译）"
	@echo "make gui-fltk 编译 C++/FLTK 图形版（需 FLTK 开发库）"
	@echo "make test    跑全部测试（C++ 29 项 + Python 28 项）"
	@echo "make install 装到 /usr/local/bin"
