CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror
TOOLS = ra gen storage
TESTS = ratest storagetest

all: $(TOOLS)

ra: src/ra.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

gen: src/gen.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

storage: src/storage.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

ratest: tests/test.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

storagetest: tests/test_storage.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: $(TESTS)
	./ratest
	./storagetest

perf: ra gen
	sh perf/run.sh > perf/results.tsv && python3 perf/plot.py perf/results.tsv

clean:
	rm -f $(TOOLS) $(TESTS)

.PHONY: all test perf clean
