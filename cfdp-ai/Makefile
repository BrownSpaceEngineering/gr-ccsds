# Makefile — CFDP Basic Implementation
#
# Targets:
#   make          — build all binaries
#   make test     — run PDU unit tests
#   make loopback — run a local sender --> receiver loopback test
#   make clean    — remove build artifacts

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wstrict-prototypes \
          -g -fsanitize=address,undefined \
          -Iinclude
LDFLAGS = -fsanitize=address,undefined

SRC = src/cfdp_pdu.c \
      src/cfdp_checksum.c \
      src/cfdp_sender.c \
      src/cfdp_receiver.c \
      src/cfdp_transport_udp.c \
      src/cfdp_util.c \
	  src/cfdp_class2_pdu.c \
	  src/cfdp_class2_gaps.c \
	  src/cfdp_class2_sender.c \
	  src/cfdp_class2_receiver.c

OBJ = $(patsubst src/%.c,build/%.o,$(SRC))

.PHONY: all test loopback clean

all: bin/test_pdu bin/test_sender bin/test_receiver bin/test_sender2 bin/test_receiver2

bin build:
	mkdir -p $@

bin/test_pdu: $(OBJ) test/test_pdu.c | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) test/test_pdu.c $(LDFLAGS)

bin/test_sender: $(OBJ) test/test_sender.c | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) test/test_sender.c $(LDFLAGS)

bin/test_receiver: $(OBJ) test/test_receiver.c | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) test/test_receiver.c $(LDFLAGS)

bin/test_sender2: $(OBJ) test/test_sender2.c | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) test/test_sender2.c $(LDFLAGS)

bin/test_receiver2: $(OBJ) test/test_receiver2.c | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) test/test_receiver2.c $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

# Run unit tests
test: bin/test_pdu
	./bin/test_pdu

# Loopback test: fork receiver, wait a moment, run sender
# Usage: make loopback FILE=myfile.bin LOSS=10
FILE ?= /etc/hostname
LOSS ?= 0

# Class 1 Loopback test
loopback: all
	@echo "--- Starting receiver (background) ---"
	./bin/test_receiver &
	RECV_PID=$$!; \
	sleep 0.4; \
	echo "--- Starting sender (file=$(FILE), loss=$(LOSS)%) ---"; \
	./bin/test_sender $(FILE) $(LOSS); \
	SEND_RC=$$?; \
	sleep 0.5; \
	kill $$RECV_PID 2>/dev/null; \
	wait $$RECV_PID 2>/dev/null; \
	if [ $$SEND_RC -eq 0 ] && diff -q $(FILE) received_file.bin >/dev/null 2>&1; then \
	    echo ""; \
	    echo "=== LOOPBACK TEST PASSED: files match ==="; \
	else \
	    echo ""; \
	    echo "=== LOOPBACK TEST FAILED ==="; \
	    exit 1; \
	fi

# Class 2 loopback test
loopback2: all
	@echo "--- Starting Class 2 receiver (background) ---"
	./bin/test_receiver2 &
	RECV_PID=$$!; \
	sleep 0.4; \
	echo "--- Starting Class 2 sender (file=$(FILE), loss=$(LOSS)%) ---"; \
	./bin/test_sender2 $(FILE) $(LOSS); \
	SEND_RC=$$?; \
	sleep 1; \
	kill $$RECV_PID 2>/dev/null; \
	wait $$RECV_PID 2>/dev/null; \
	if [ $$SEND_RC -eq 0 ] && diff -q $(FILE) received_file.bin >/dev/null 2>&1; then \
	    echo ""; \
	    echo "=== CLASS 2 LOOPBACK TEST PASSED: files match ==="; \
	else \
	    echo ""; \
	    echo "=== CLASS 2 LOOPBACK TEST FAILED ==="; \
	    exit 1; \
	fi

clean:
	rm -rf build bin
	rm -f $(SRC:.c=.o)
	rm -f received_file.bin
