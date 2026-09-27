CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra

SRC = $(wildcard src/*.cpp)
OBJ = $(patsubst src/%.cpp,build/%.o,$(SRC))

NAME = spp

.PHONY: all exe run clean

all: bin/$(NAME)

run: bin/$(NAME)
	./bin/$(NAME)

bin/$(NAME): $(OBJ)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ -c $<

clean:
	rm -rf build bin
