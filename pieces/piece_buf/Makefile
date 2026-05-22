# ================================================
# PieceTable Makefile
# ================================================

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -g
AR = ar
ARFLAGS = rcs

LIB_SRC = piece_table.c
LIB_OBJ = piece_table.o
LIB_NAME = libpiece_table.a

TEST_SRC = piece_table_tests.c
TEST_EXE = piece_table_tests

# Default target
all: lib test

# Build static library
lib: $(LIB_NAME)

$(LIB_NAME): $(LIB_OBJ)
	$(AR) $(ARFLAGS) $@ $^
	@echo "Static library created: $@"

$(LIB_OBJ): $(LIB_SRC) piece_table.h
	$(CC) $(CFLAGS) -c $< -o $@

# Build test executable
test: $(TEST_EXE)

$(TEST_EXE): $(TEST_SRC) $(LIB_OBJ) piece_table.h
	$(CC) $(CFLAGS) $(TEST_SRC) $(LIB_OBJ) -o $@
	@echo "Test executable built: $@"

# Run tests
run: test
	./$(TEST_EXE)

# Clean
clean:
	rm -f *.o *.a $(TEST_EXE) test_*.txt edited.txt
	@echo "Clean completed"

rebuild: clean all

.PHONY: all lib test run clean rebuild
