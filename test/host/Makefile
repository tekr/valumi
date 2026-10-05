# Host-side unit tests for the ticker's pure logic.
#
#   make -C test/host        build and run everything
#
# Each module under test is plain C with no ESP-IDF dependency (or, for the
# carousel, only a mutex and a clock, which shim/ fakes). cJSON is compiled
# straight from the ESP-IDF tree so the tests parse JSON exactly as the device
# does.

IDF_PATH ?= $(HOME)/esp/esp-idf
MAIN := ../../main
CJSON := $(IDF_PATH)/components/json/cJSON

# -Wno-deprecated-declarations: cJSON uses sprintf, which the macOS SDK flags.
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
          -fno-omit-frame-pointer -Wno-deprecated-declarations -Ishim -I$(MAIN) -I$(CJSON)
LDFLAGS := -fsanitize=address,undefined

BUILD := build
TESTS := test_settings test_button_seq test_auth_core test_captive_dns test_local_time \
         test_wifi_qr test_carousel test_screens test_market_core test_chart_path test_wifi_policy

SRC_test_settings := $(MAIN)/settings.c $(MAIN)/settings_json.c $(MAIN)/code.c $(CJSON)/cJSON.c
SRC_test_button_seq := $(MAIN)/button_seq.c
SRC_test_auth_core := $(MAIN)/auth_core.c
SRC_test_captive_dns := $(MAIN)/captive_dns_core.c
SRC_test_local_time := $(MAIN)/local_time.c
SRC_test_wifi_qr := $(MAIN)/wifi_qr.c
SRC_test_carousel := $(MAIN)/carousel.c shim/shim.c
SRC_test_wifi_policy := $(MAIN)/wifi_policy.c
SRC_test_screens := $(MAIN)/screens.c $(MAIN)/wifi_qr.c
SRC_test_market_core := $(MAIN)/market_core.c
SRC_test_chart_path := $(MAIN)/chart_path.c

.PHONY: all run clean
all: run

$(BUILD):
	mkdir -p $(BUILD)

.SECONDEXPANSION:
# Every header in main/ and shim/ is a prerequisite of every test. Coarse, but
# without it editing a header (a limit in settings.h, say) leaves a stale test
# binary that still passes; per-file dependency files do not work here because
# each test compiles several sources in one command.
HEADERS := $(wildcard $(MAIN)/*.h) $(wildcard shim/*.h shim/freertos/*.h)

$(BUILD)/%: %.c test.h $$(SRC_%) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRC_$*) $(LDFLAGS)

run: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $^; do $$t; done; echo "ALL HOST TESTS PASSED"

clean:
	rm -rf $(BUILD)
