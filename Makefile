CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror
TOOLS = ra gen storage exec sql
TESTS = ratest storagetest exectest sqltest

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

exec: src/exec.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

exectest: tests/test_exec.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

sql: src/sql.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

sqltest: tests/test_sql.cpp src/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: $(TESTS)
	./ratest
	./storagetest
	./exectest
	./sqltest

perf: ra gen
	sh perf/run.sh > perf/results.tsv && python3 perf/plot.py perf/results.tsv

clean:
	rm -f $(TOOLS) $(TESTS)

.PHONY: all test perf clean
