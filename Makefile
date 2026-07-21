# Host test build. The library itself has no build system to speak of —
# vendor include/ + src/ into your firmware tree or add them to your build.
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -g -O1
CPPFLAGS = -Iinclude -Itests
LDLIBS = -lm

SRC = src/wire.c src/doc.c src/desc.c src/codec.c src/device.c src/command.c
TESTS = test_codec test_doc test_device test_command
PY = ../moddef-py/.venv/bin/python
GEN = $(BUILD)/gen

BUILD = build

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: tests/%.c $(SRC) $(wildcard include/moddef/*.h) $(wildcard tests/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(SRC) $(LDLIBS)

# Test documents: converted from JSON/YAML with moddef-py (host-side only).
$(BUILD)/meter.moddef: tests/data/meter.json tools/convert.py | $(BUILD)
	$(PY) tools/convert.py $< $@

$(BUILD)/sunspec.moddef: tests/data/sunspec.json tools/convert.py | $(BUILD)
	$(PY) tools/convert.py $< $@

$(BUILD)/commands.moddef: tests/data/commands.json tools/convert.py | $(BUILD)
	$(PY) tools/convert.py $< $@

$(BUILD)/growatt-sph.moddef: ../devices/solar-inverter/growatt-sph/growatt-sph.moddef.yaml tools/convert.py | $(BUILD)
	$(PY) tools/convert.py $< $@

# Generated modules (spec §31): embedded binary document + typed accessors.
$(GEN)/growatt_sph.h $(GEN)/growatt_sph.c: ../devices/solar-inverter/growatt-sph/growatt-sph.moddef.yaml tools/moddef_c_gen.py | $(BUILD)
	$(PY) tools/moddef_c_gen.py $< -o $(GEN)

$(GEN)/test_sunspec.h $(GEN)/test_sunspec.c: tests/data/sunspec.json tools/moddef_c_gen.py | $(BUILD)
	$(PY) tools/moddef_c_gen.py $< -o $(GEN)

$(BUILD)/test_generated: tests/test_generated.c $(SRC) $(GEN)/growatt_sph.c $(GEN)/test_sunspec.c $(wildcard include/moddef/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(GEN) -o $@ $< $(SRC) $(GEN)/growatt_sph.c $(GEN)/test_sunspec.c $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/test_generated $(BUILD)/meter.moddef $(BUILD)/sunspec.moddef $(BUILD)/commands.moddef $(BUILD)/growatt-sph.moddef
	$(BUILD)/test_codec
	$(BUILD)/test_doc
	$(BUILD)/test_device
	$(BUILD)/test_command
	$(BUILD)/test_generated

examples: $(BUILD)/superloop

$(BUILD)/superloop: examples/superloop.c $(SRC) $(GEN)/growatt_sph.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(GEN) -o $@ $< $(SRC) $(GEN)/growatt_sph.c $(LDLIBS)

# Cross-compile smoke: the runtime and a generated module must build for a
# Cortex-M4 (no std beyond string.h/math.h, no allocation).
THUMB_CC = arm-none-eabi-gcc
THUMB_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -Os -mcpu=cortex-m4 -mthumb \
  -ffunction-sections -fdata-sections -Iinclude

thumb: $(GEN)/growatt_sph.c | $(BUILD)
	set -e; for f in $(SRC) $(GEN)/growatt_sph.c; do \
	  $(THUMB_CC) $(THUMB_FLAGS) -I$(GEN) -c $$f -o $(BUILD)/thumb_$$(basename $$f .c).o; \
	done
	@echo "thumb build OK"

clean:
	rm -rf $(BUILD)

.PHONY: all test examples thumb clean
