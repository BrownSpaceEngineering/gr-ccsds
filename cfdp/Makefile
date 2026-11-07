CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -pthread
LDLIBS = -pthread
CPPFLAGS = -I..

SRC = $(wildcard src/*.cpp)
COMMON_SRC = $(wildcard ../common/*.cpp)
OBJ = $(patsubst src/%.cpp,build/%.o,$(SRC)) $(patsubst ../common/%.cpp,build/common/%.o,$(COMMON_SRC))
TEST_OBJ = $(filter-out build/main.o,$(OBJ))
TEST_SRC = test/test_cfdp.cpp test/test_entity.cpp test/notebook_core.cpp test/test_completion.cpp test/test_persistence.cpp test/test_disk_filestore.cpp test/test_async_storage.cpp
TEST_BIN = $(patsubst test/%.cpp,bin/%,$(TEST_SRC))

NAME = cfdp

.PHONY: all run test test-udp test-interop clean

all: bin/$(NAME)

run: bin/$(NAME)
	./bin/$(NAME)

test: $(TEST_BIN)
	@set -e; for test_bin in $(TEST_BIN); do ./$$test_bin; done

test-udp: bin/$(NAME) bin/test_udp_receiver
	./bin/test_udp_receiver
	uv run test/test_udp.py

test-interop: bin/$(NAME)
	uv run test/test_interop.py

bin/$(NAME): $(OBJ)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^ $(LDLIBS)

bin/test_%: test/test_%.cpp $(TEST_OBJ)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isrc -o $@ $^

bin/notebook_core: test/notebook_core.cpp $(TEST_OBJ)
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
