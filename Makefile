CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror

all: ra gen

ra: src/ra.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

gen: src/gen.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

ratest: tests/test.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: ratest
	./ratest

perf: ra gen
	sh perf/run.sh > perf/results.tsv && python3 perf/plot.py perf/results.tsv

clean:
	rm -f ra gen ratest

.PHONY: all test perf clean
