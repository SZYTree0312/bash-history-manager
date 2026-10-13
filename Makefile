CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

bhm: bash_history_manager.cpp
	$(CXX) $(CXXFLAGS) -o bhm bash_history_manager.cpp

install: bhm
	install -m 755 bhm /usr/local/bin/bhm

test: bhm
	bash tests/run.sh

clean:
	rm -f bhm

.PHONY: install test clean
