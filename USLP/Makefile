CXX := g++
CXXFLAGS := -std=c++20 -O0 -Wall -Wextra -I src/common/include -I CRCpp/inc -MMD -MP

# Source files
COMMON_SRCS := $(wildcard src/common/src/*.cpp)

# Object files
OBJS := $(patsubst src/common/src/%.cpp, build/%.o, $(COMMON_SRCS))

# Dependency files
DEPS := $(OBJS:.o=.d)

# Programs to build
PROGS := uslp packing utils

.PHONY: all clean

all: $(PROGS)

# Build programs
uslp: build/uslp.o $(OBJS) | bin
	$(CXX) $^ -o bin/$@

packing: build/packing.o $(OBJS) | bin
	$(CXX) $^ -o bin/$@

utils: build/utils.o $(OBJS) | bin
	$(CXX) $^ -o bin/$@

# Compile common object files
build/%.o: src/common/src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Programs specific object files
build/uslp.o: src/common/src/uslp.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/packing.o: src/common/src/packing.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/utils.o: src/common/src/utils.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Create bin directory
bin:
	@mkdir -p bin

# Clean build artifacts
clean:
	rm -rf build bin

# Include dependency files
-include $(DEPS)