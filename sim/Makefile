# Standalone build of the pokefirered battle simulator (host compiler).
CC      ?= clang
BUILD   ?= build
ROOT    := ..
OPT     ?= -O2 -g
CFLAGS  := $(OPT)
CFLAGS  += -std=gnu11 -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1 -DSIM_BUILD=1
CFLAGS  += -I include -I gen -I tests -I $(ROOT)/include -I $(ROOT)/src
CFLAGS  += -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-parentheses -Wno-switch -Wno-misleading-indentation -Wno-implicit-fallthrough \
           -Wno-unused-value -Wno-pointer-sign -Wno-int-conversion -Wno-incompatible-pointer-types-discards-qualifiers \
           -Wno-tautological-constant-out-of-range-compare -Wno-unknown-attributes -Wno-gnu -Wno-dangling-else \
           -Wno-unused-label -Wno-address-of-packed-member -Wno-missing-braces -Wno-bitwise-op-parentheses \
           -Wno-logical-op-parentheses -Wno-shift-op-parentheses -Wno-string-plus-int -Wno-sign-compare \
           -Wno-unused-parameter -Wno-empty-body -Wno-self-assign -Wno-implicit-function-declaration -ferror-limit=300

GEN_C   := gen/battle_scripts.c gen/battle_scripts_externs.c gen/string_tables.c gen/items_table.c gen/names.c gen/pokedex_hw.c
SRC_C   := $(wildcard src/*.c)
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(SRC_C) $(GEN_C))

TEST_SRCS := $(filter-out tests/scenario.c tests/scenarios_main.c,$(wildcard tests/*.c))
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS)) $(BUILD)/scenarios
SCENARIO_SRCS := $(wildcard tests/scenarios/*.c)
SCENARIO_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SCENARIO_SRCS)) $(BUILD)/tests/scenario.o $(BUILD)/gen/scenario_list.o

all: $(BUILD)/libsim.a $(TESTS)

# Shared library for Python (py/frlgsim loads build/libfrlgsim.dylib).
lib: $(BUILD)/libfrlgsim.dylib
$(BUILD)/libfrlgsim.dylib: $(OBJS)
	@$(CC) -shared -o $@ $^

scenarios: $(BUILD)/scenarios

gen/battle_scripts.c include/battle_scripts.h: tools/bsasm.py $(ROOT)/data/battle_scripts_1.s $(ROOT)/data/battle_scripts_2.s $(ROOT)/asm/macros/battle_script.inc
	python3 tools/bsasm.py gen/battle_scripts.c include/battle_scripts.h $(ROOT)/data/battle_scripts_1.s $(ROOT)/data/battle_scripts_2.s

gen/sim_state_fields.h gen/sim_globals_gen.h: tools/gen_state.py
	python3 tools/gen_state.py gen

PREPROC := $(ROOT)/tools/preproc/preproc

$(BUILD)/%.o: %.c include/battle_scripts.h gen/sim_state_fields.h gen/sim_globals_gen.h include/sim.h include/sim_globals.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -E $< | $(PREPROC) -i $< $(ROOT)/charmap.txt | $(CC) $(CFLAGS) -Wno-unused-command-line-argument -x c -c - -o $@

$(BUILD)/libsim.a: $(OBJS)
	ar rcs $@ $^

gen/pokedex_hw.c: tools/gen_pokedex.py $(ROOT)/src/data/pokemon/pokedex_entries.h
	python3 tools/gen_pokedex.py gen/pokedex_hw.c

gen/scenario_list.c: tools/gen_scenarios.py $(SCENARIO_SRCS)
	python3 tools/gen_scenarios.py $@

$(BUILD)/tests/%.o: tests/%.c tests/scenario.h include/sim.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -I tests -c $< -o $@

$(BUILD)/gen/scenario_list.o: gen/scenario_list.c tests/scenario.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -I tests -c $< -o $@

$(BUILD)/scenarios: tests/scenarios_main.c $(SCENARIO_OBJS) $(BUILD)/libsim.a
	@$(CC) $(CFLAGS) -I tests $< $(SCENARIO_OBJS) $(BUILD)/libsim.a -o $@

$(BUILD)/crosscheck: tests/crosscheck.c $(SCENARIO_OBJS) $(BUILD)/libsim.a
	@$(CC) $(CFLAGS) -I tests $< $(SCENARIO_OBJS) $(BUILD)/libsim.a -o $@

$(BUILD)/%: tests/%.c $(BUILD)/libsim.a
	@$(CC) $(CFLAGS) $< $(BUILD)/libsim.a -o $@

test: all
	./$(BUILD)/mechanics
	./$(BUILD)/scenarios
	./$(BUILD)/fuzz 2000 1
	./$(BUILD)/alltrainers 1
	./$(BUILD)/robust 3000 1

# Replays the recorded ROM transcripts in corpus/ against the simulator (seconds, no mGBA needed).
backtest: $(BUILD)/crosscheck
	./tools/backtest.sh

# Robustness fuzz under AddressSanitizer + UBSan (separate build dir; the first build takes a while).
ROBUST_N ?= 20000
robust:
	$(MAKE) BUILD=build_asan OPT="-O1 -g -fsanitize=address,undefined -fno-sanitize=shift,alignment -fno-omit-frame-pointer" build_asan/robust
	ASAN_OPTIONS=detect_leaks=0 ./build_asan/robust $(ROBUST_N) 1

clean:
	rm -rf build build_asan

.PHONY: all clean test scenarios backtest robust lib
