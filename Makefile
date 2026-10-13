CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

bhm: bash_history_manager.cpp
	$(CXX) $(CXXFLAGS) -o bhm bash_history_manager.cpp

# GUI 版需要 FLTK 1.4（Linux: apt install libfltk-dev；源码编译见 README）
FLTK_CONFIG ?= fltk-config
FLTK_CXXFLAGS := $(shell $(FLTK_CONFIG) --cxxflags 2>/dev/null)
FLTK_LIBS := $(shell $(FLTK_CONFIG) --ldflags 2>/dev/null)

bhm-gui: bhm_gui.cpp
	$(CXX) $(CXXFLAGS) $(FLTK_CXXFLAGS) -o bhm-gui bhm_gui.cpp $(FLTK_LIBS)

all: bhm bhm-gui

install: bhm
	install -m 755 bhm /usr/local/bin/bhm

test: bhm
	bash tests/run.sh

clean:
	rm -f bhm bhm-gui

.PHONY: all install test clean
