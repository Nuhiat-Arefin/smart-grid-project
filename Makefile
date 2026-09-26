CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Icpp
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := build
CPP_SOURCES := $(wildcard cpp/*.cpp)
CORE_SOURCES := $(filter-out cpp/main.cpp cpp/server.cpp,$(CPP_SOURCES))
APP_SOURCES := $(CORE_SOURCES) cpp/main.cpp cpp/server.cpp

.PHONY: all test dashboard-test clean

all: $(BUILD_DIR)/smartgrid

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/smartgrid: $(APP_SOURCES) cpp/json.hpp cpp/smartgrid.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(APP_SOURCES) -o $@ $(LDLIBS)

$(BUILD_DIR)/test_smartgrid: $(CORE_SOURCES) tests/test_cpp.cpp cpp/json.hpp cpp/smartgrid.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(CORE_SOURCES) tests/test_cpp.cpp -o $@ $(LDLIBS)


$(BUILD_DIR)/test_algorithms: tests/test_cpp_algorithms.cpp cpp/core.cpp cpp/json.hpp cpp/smartgrid.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) tests/test_cpp_algorithms.cpp -o $@ $(LDLIBS)

test: $(BUILD_DIR)/test_algorithms $(BUILD_DIR)/test_smartgrid
	$(BUILD_DIR)/test_algorithms
	$(BUILD_DIR)/test_smartgrid

# Browser regression checks are optional because they require a local Node.js
# runtime; the tests use an isolated VM harness.
dashboard-test: $(BUILD_DIR)/smartgrid
	node --test tests/test_dashboard.js

clean:
	rm -f $(BUILD_DIR)/smartgrid $(BUILD_DIR)/test_algorithms $(BUILD_DIR)/test_smartgrid
	rmdir $(BUILD_DIR) 2>/dev/null || true
