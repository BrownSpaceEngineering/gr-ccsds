CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra
CPPFLAGS = -I..

SRC = $(wildcard src/*.cpp)
COMMON_SRC = $(wildcard ../common/*.cpp)
OBJ = $(patsubst src/%.cpp,build/%.o,$(SRC)) $(patsubst ../common/%.cpp,build/common/%.o,$(COMMON_SRC))
TEST_OBJ = $(filter-out build/main.o,$(OBJ))
TEST_SRC = $(wildcard test/*.cpp)
TEST_BIN = $(patsubst test/%.cpp,bin/%,$(TEST_SRC))

NAME = spp

.PHONY: all run test clean

all: bin/$(NAME)

run: bin/$(NAME)
	./bin/$(NAME)

test: $(TEST_BIN)
	@set -e; for test_bin in $(TEST_BIN); do ./$$test_bin; done

bin/$(NAME): $(OBJ)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^

bin/test_%: test/test_%.cpp $(TEST_OBJ)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isrc -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -o $@ -c $<

build/common/%.o: ../common/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -o $@ -c $<

-include $(OBJ:.o=.d)

clean:
	rm -rf build bin
