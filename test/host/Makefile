# Host tests of the firmware logic: plain gcc, no ESP-IDF or board needed.
#   make -C test/host          build and run
#   make -C test/host VERBOSE=1 with the modules' log output
# cJSON comes from managed_components (the same version as in the firmware): after a fresh
# clone run "idf.py reconfigure" once to download it.

ROOT    := ../..
MAIN    := $(ROOT)/main
CJSON   := $(ROOT)/managed_components/espressif__cjson/cJSON
BUILD   := build

# Leaks are not checked: tests keep their cJSON documents, modules their static buffers
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS  := -std=gnu17 -g -O1 $(SAN) -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare
INCLUDE := -I. -Istubs -I$(MAIN) -I$(CJSON)
# The track directory STORAGE_BASE_PATH is faked in stubs.c
WRAP    := -Wl,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=stat

TESTS   := test_settings

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

run-%: $(BUILD)/%
	@echo "== $*"
	@VERBOSE=$(VERBOSE) ASAN_OPTIONS=detect_leaks=0 $<

$(BUILD)/cJSON.o: $(CJSON)/cJSON.c | $(BUILD)
	$(CC) -std=gnu17 -g -O1 $(SAN) -c -o $@ $<

$(BUILD)/test_settings: test_settings.c $(MAIN)/settings.c $(MAIN)/storage.c stubs/stubs.c $(BUILD)/cJSON.o \
                        $(wildcard stubs/*.h stubs/*/*.h) test.h $(MAIN)/settings.h $(MAIN)/storage.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDE) $(WRAP) -o $@ test_settings.c $(MAIN)/settings.c $(MAIN)/storage.c \
	      stubs/stubs.c $(BUILD)/cJSON.o -lm

$(BUILD):
	@test -f $(CJSON)/cJSON.c || { echo "cJSON not found: run idf.py reconfigure once"; exit 1; }
	mkdir -p $@

clean:
	rm -rf $(BUILD)
