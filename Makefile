
CXX = g++

CXXFLAGS = -std=c++17 -I../include -I/usr/include

LIBS = -lgtest -lgtest_main -lpthread -lboost_thread -lboost_system -ltcmalloc -lprofiler

TARGET = main

SRCS = tests/main.cpp src/heap_page_cache.cpp src/heap_file.cpp

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

tests: all
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf tmp/*
	rm -f profile.pdf profile.svg profile.prof

.PHONY: all clean
