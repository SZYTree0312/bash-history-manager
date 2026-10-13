# bhm — bash/zsh 历史文件管理器
#
# 终端版（bhm）：C++17，零依赖。Linux 上一条 g++ 就出。
# 图形版（bhm_gui.py）：Python 3 + tkinter，免编译 —— 不依赖任何 GUI 开发库。

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PY ?= python3

.PHONY: all bhm gui test test-cli test-gui install clean help

all: bhm

bhm: bash_history_manager.cpp
	$(CXX) $(CXXFLAGS) -o bhm bash_history_manager.cpp

# 图形版是 Python 的，不需要编译
gui:
	$(PY) bhm_gui.py

test-cli: bhm
	bash tests/run.sh

test-gui:
	$(PY) tests/test_gui_core.py

test: test-cli test-gui

install: bhm
	install -m 755 bhm /usr/local/bin/bhm
	install -m 755 bhm_gui.py /usr/local/bin/bhm-gui
	@echo "装好了: bhm（终端版）、bhm-gui（图形版，直接敲就行）"

clean:
	rm -f bhm
	rm -rf __pycache__ tests/__pycache__

help:
	@echo "make         编译终端版 bhm（g++，零依赖）"
	@echo "make gui     启动 Python 图形版（免编译）"
	@echo "make test    跑全部测试（C++ 29 项 + Python 28 项）"
	@echo "make install 装到 /usr/local/bin（bhm + bhm-gui）"
