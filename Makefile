CFLAGS ?= -Wall -ggdb -W -O
INCLUDES ?= -Iinclude
CC ?= gcc
LIBS ?= -lssl -lcrypto
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

test_arguments.o: test/test_arguments.c include/arguments.h
	$(CC) $(CFLAGS) $(INCLUDES) -o ${TARGET_TEST_DIR}test_arguments.o -c test/test_arguments.c $(TEST_LIBS)

arguments.o: prepare include/arguments.h src/arguments.c
	$(CC) $(CFLAGS) $(INCLUDES) -c src/arguments.c -o $(TARGET_DIR)arguments.o

webbench2.o: prepare src/webbench2.c include/arguments.h
	$(CC) $(CFLAGS) $(INCLUDES) -c src/webbench2.c -o $(TARGET_DIR)webbench2.o
	@echo "Compiled webbench2.o successfully."

$(TARGET): prepare test_arguments webbench2.o arguments.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET_DIR)$(TARGET) $(TARGET_DIR)webbench2.o $(TARGET_DIR)arguments.o $(TEST_LIBS)
	@echo "WebBench 2 compiled successfully."

clean:
	rm -rf $(TARGET_DIR)
	@echo "Cleaned up all generated files."