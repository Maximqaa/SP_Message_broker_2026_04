CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
LDFLAGS = -pthread
LDLIBS =

ifdef USE_SQLITE
    CXXFLAGS += -DUSE_SQLITE
    LDLIBS += -lsqlite3
endif

.PHONY: all clean test

all: server_enhanced interactive_client test_client

server_enhanced: server_enhanced.cpp subscription_manager.cpp logger.h config.h stats.h stats.cpp
        $(CXX) $(CXXFLAGS) -o $@ server_enhanced.cpp subscription_manager.cpp stats.cpp $(LDFLAGS) $(LDLIBS)

interactive_client: interactive_client.cpp
        $(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_client: test_client.cpp
        $(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
        rm -f server_enhanced interactive_client test_client *.o

test: all
        @bash test.sh
