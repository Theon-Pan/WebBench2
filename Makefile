CFLAGS ?= -Wall -ggdb -W -O
INCLUDES ?= -Iinclude
CC ?= gcc
LIBS ?= -lssl -lcrypto -lpthread
TEST_LIBS ?= -lcheck
LDFLAGS ?=
PREFIX ?= /usr/local/webbench2
VERSION = 2.0
TMPDIR = /tmp/webbench2-$(VERSION)
TARGET_DIR = ./target/
TARGET_TEST_DIR = ./target/test/
TARGET = webbench2

.PHONY: test_arguments, clean, all, $(TARGET),prepare

all: clean prepare $(TARGET)

prepare:
	mkdir -p $(TARGET_DIR)
	mkdir -p $(TARGET_TEST_DIR)

test_arguments: test_arguments.o arguments.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET_TEST_DIR)test_arguments $(TARGET_DIR)arguments.o $(TARGET_TEST_DIR)test_arguments.o $(TEST_LIBS)
	$(TARGET_TEST_DIR)test_arguments

test_request: test_request.o request.o arguments.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET_TEST_DIR)test_request $(TARGET_DIR)request.o $(TARGET_DIR)arguments.o $(TARGET_TEST_DIR)test_request.o $(TEST_LIBS)
	$(TARGET_TEST_DIR)test_request

test_arguments.o: test/test_arguments.c include/arguments.h
	$(CC) $(CFLAGS) $(INCLUDES) -o ${TARGET_TEST_DIR}test_arguments.o -c test/test_arguments.c $(TEST_LIBS)

test_request.o: test/test_request.c include/request.h include/arguments.h
	$(CC) $(CFLAGS) $(INCLUDES) -o ${TARGET_TEST_DIR}test_request.o -c test/test_request.c ${TEST_LIBS}

arguments.o: prepare include/arguments.h src/arguments.c
	$(CC) $(CFLAGS) $(INCLUDES) -c src/arguments.c -o $(TARGET_DIR)arguments.o

request.o: prepare include/request.h src/request.c
	$(CC) $(CFLAGS) $(INCLUDES) -c src/request.c -o $(TARGET_DIR)request.o

bench2.o: prepare include/bench2.h src/bench2.c include/communicator.h
	$(CC) $(CFLAGS) $(INCLUDES) -c src/bench2.c -o $(TARGET_DIR)bench2.o

communicator.o: prepare include/communicator.h src/communicator.c
	$(CC) $(CFLAGS) $(INCLUDES) -c src/communicator.c -o $(TARGET_DIR)communicator.o

bench_select.o: prepare include/bench_select.h src/bench_select.c
	$(CC) $(CFLAGS) $(INCLUDES) -c src/bench_select.c -o $(TARGET_DIR)bench_select.o

webbench2.o: prepare src/webbench2.c include/arguments.h
	$(CC) $(CFLAGS) $(INCLUDES) -c src/webbench2.c -o $(TARGET_DIR)webbench2.o
	@echo "Compiled webbench2.o successfully."

$(TARGET): prepare test_arguments test_request webbench2.o arguments.o request.o bench2.o communicator.o bench_select.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET_DIR)$(TARGET) $(TARGET_DIR)webbench2.o $(TARGET_DIR)arguments.o $(TARGET_DIR)request.o $(TARGET_DIR)bench2.o $(TARGET_DIR)communicator.o $(TARGET_DIR)bench_select.o $(LIBS)
	@echo "WebBench 2 compiled successfully."

clean:
	rm -rf $(TARGET_DIR)
	@echo "Cleaned up all generated files."