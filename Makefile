CXX = g++

CXXFLAGS = -std=c++17 -I./include -I/usr/include -O2

LIBS = -lgtest -lgtest_main -lpthread -lboost_thread -lboost_system -ltcmalloc -lprofiler

# Targets
TARGET = main
MIRA_TARGET = mira_test
CACHE_BENCHMARK = cache_benchmark

# Source files
SRCS = tests/main.cpp src/heap_page_cache.cpp src/heap_file.cpp
MIRA_SRCS = tests/mira_test.cpp src/mira_page_cache.cpp src/heap_file.cpp
BENCHMARK_SRCS = tests/cache_benchmark.cpp src/heap_page_cache.cpp src/mira_page_cache.cpp src/heap_file.cpp

# Object files
OBJS = $(SRCS:.cpp=.o)
MIRA_OBJS = $(MIRA_SRCS:.cpp=.o)
BENCHMARK_OBJS = $(BENCHMARK_SRCS:.cpp=.o)

# Build all targets
all: $(TARGET) $(MIRA_TARGET) $(CACHE_BENCHMARK)

# Main test target
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

# Mira test target
$(MIRA_TARGET): $(MIRA_OBJS)
	$(CXX) $(CXXFLAGS) -o $(MIRA_TARGET) $(MIRA_SRCS) $(LIBS)

# Cache benchmark target
$(CACHE_BENCHMARK): $(BENCHMARK_OBJS)
	$(CXX) $(CXXFLAGS) -o $(CACHE_BENCHMARK) $(BENCHMARK_SRCS) $(LIBS)

# Pattern rule for test files
tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pattern rule for source files
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Run tests
tests: $(TARGET)
	./$(TARGET)

# Run Mira tests
mira_tests: $(MIRA_TARGET)
	./$(MIRA_TARGET)

# Run cache benchmark
benchmark: $(CACHE_BENCHMARK)
	./$(CACHE_BENCHMARK)

# Create necessary directories
.PHONY: init
init:
	mkdir -p tmp

# Clean up build artifacts and temporary files
clean:
	rm -f $(TARGET) $(MIRA_TARGET) $(CACHE_BENCHMARK)
	rm -f $(OBJS) $(MIRA_OBJS) $(BENCHMARK_OBJS)
	rm -rf tmp/*
	rm -f profile.pdf profile.svg profile.prof
	rm -f cache_benchmark_results.txt

# Clean and rebuild everything
rebuild: clean all

.PHONY: all clean tests mira_tests benchmark rebuild init