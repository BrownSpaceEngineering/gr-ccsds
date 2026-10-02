CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra

SRC = $(wildcard src/*.cpp)
OBJ = $(patsubst src/%.cpp,build/%.o,$(SRC))
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
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ -c $<

clean:
	rm -rf build bin
