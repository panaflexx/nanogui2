# ============================================================================
#  PieceTable - portable Makefile
#
#  Targets
#  -------
#    all       (default)  - build static library + test executable
#    static                - build libpiece_table.a only
#    shared                - build the shared library (libpiece_table.{so,dylib,dll})
#    test                  - build the test executable
#    run                   - build & run tests
#    clean
#    rebuild
#
#  Tested with gcc, clang, and (on Windows) the MinGW toolchain.  The library
#  itself is ISO C11; the test harness uses C11's timespec_get() only.
# ============================================================================

# ---- toolchain (overridable from the command line) -------------------------
CC      ?= cc
AR      ?= ar
# Make pre-defines ARFLAGS=rv, so override unconditionally for a quiet build.
ARFLAGS  := rcs
RM      ?= rm -f

# ---- flags -----------------------------------------------------------------
STD       ?= -std=c11
OPT       ?= -O2 -g
WARN       = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
             -Wmissing-prototypes -Wpointer-arith -Wcast-align
CFLAGS    ?= $(OPT) $(WARN) $(STD)

# Enable hidden-by-default visibility for shared builds on GCC/clang.
PIC_FLAGS  = -fPIC
VIS_FLAGS  = -fvisibility=hidden

# ---- platform detection ----------------------------------------------------
UNAME_S ?= $(shell uname -s 2>/dev/null)

ifeq ($(OS),Windows_NT)
    SHLIB_EXT := dll
    EXE_EXT   := .exe
    SHLIB_FLAGS := -shared
    PIC_FLAGS   :=
    VIS_FLAGS   :=
else ifeq ($(UNAME_S),Darwin)
    SHLIB_EXT := dylib
    EXE_EXT   :=
    SHLIB_FLAGS := -dynamiclib
else
    SHLIB_EXT := so
    EXE_EXT   :=
    SHLIB_FLAGS := -shared
endif

# ---- files -----------------------------------------------------------------
LIB_SRC    := piece_table.c
LIB_HDR    := piece_table.h
LIB_OBJ    := piece_table.o
LIB_PIC_OBJ:= piece_table.pic.o

STATIC_LIB := libpiece_table.a
SHARED_LIB := libpiece_table.$(SHLIB_EXT)

TEST_SRC   := piece_table_tests.c
TEST_EXE   := piece_table_tests$(EXE_EXT)

# ---- default target --------------------------------------------------------
.PHONY: all static shared test run clean rebuild
all: static test

# ---- static library --------------------------------------------------------
static: $(STATIC_LIB)

$(LIB_OBJ): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJ)
	$(AR) $(ARFLAGS) $@ $^
	@echo "[ar]  $@"

# ---- shared library --------------------------------------------------------
shared: $(SHARED_LIB)

$(LIB_PIC_OBJ): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) $(PIC_FLAGS) $(VIS_FLAGS) -DPT_BUILD_SHARED -c $< -o $@

$(SHARED_LIB): $(LIB_PIC_OBJ)
	$(CC) $(SHLIB_FLAGS) -o $@ $^
	@echo "[ld]  $@"

# ---- test executable -------------------------------------------------------
test: $(TEST_EXE)

$(TEST_EXE): $(TEST_SRC) $(STATIC_LIB) $(LIB_HDR)
	$(CC) $(CFLAGS) $(TEST_SRC) $(STATIC_LIB) -o $@
	@echo "[cc]  $@"

run: $(TEST_EXE)
	./$(TEST_EXE)

# ---- housekeeping ----------------------------------------------------------
clean:
	$(RM) $(LIB_OBJ) $(LIB_PIC_OBJ) $(STATIC_LIB) $(SHARED_LIB) $(TEST_EXE) \
	      test_*.txt edited.txt tmp_*.txt
	@echo "[clean]"

rebuild: clean all
