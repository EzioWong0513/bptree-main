CXX = g++

CXXFLAGS = -std=c++17 -I./include -I/usr/include

LIBS = -lgtest -lgtest_main -lpthread -lboost_thread -lboost_system -ltcmalloc -lprofiler

TARGET = main
MIRA_TARGET = mira_test

SRCS = tests/main.cpp src/heap_page_cache.cpp src/heap_file.cpp
MIRA_SRCS = tests/mira_test.cpp src/mira_page_cache.cpp src/heap_file.cpp

OBJS = $(SRCS:.cpp=.o)
MIRA_OBJS = $(MIRA_SRCS:.cpp=.o)

all: $(TARGET) $(MIRA_TARGET) $(SIMPLE_TARGET) $(MODERATE_TARGET) $(DEBUG_TARGET) $(STRESS_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

$(MIRA_TARGET): $(MIRA_OBJS)
	$(CXX) $(CXXFLAGS) -o $(MIRA_TARGET) $(MIRA_SRCS) $(LIBS)

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

tests: all
	./$(TARGET)

mira_tests: $(MIRA_TARGET)
	./$(MIRA_TARGET)

clean:
	rm -f $(TARGET) $(MIRA_TARGET) $(SIMPLE_TARGET) $(MODERATE_TARGET) $(DEBUG_TARGET) $(STRESS_TARGET) $(OBJS) $(MIRA_OBJS) $(SIMPLE_OBJS) $(MODERATE_OBJS) $(DEBUG_OBJS) $(STRESS_OBJS)
	rm -rf tmp/*
	rm -f profile.pdf profile.svg profile.prof

.PHONY: all clean tests mira_tests 