# make                 -> build/pipe-chunk
# make test            -> build + run every tests/test_*.c
# make SAN=thread test -> same under ThreadSanitizer (build-thread/); SAN=address for ASan
CFLAGS  = -std=c11 -Wall -Wextra -O2 -g -pthread -D_GNU_SOURCE -Isrc
LDLIBS  = -lcrypto -pthread
BUILD   = build

ifneq ($(SAN),)
CFLAGS  += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SAN)
BUILD   = build-$(SAN)
endif

SRC    := $(wildcard src/*.c)
OBJ    := $(SRC:src/%.c=$(BUILD)/%.o)
LIBOBJ := $(filter-out $(BUILD)/main.o,$(OBJ))
TESTS  := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))
BIN    := $(BUILD)/pipe-chunk

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_%: tests/test_%.c $(LIBOBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD):
	mkdir -p $@

test: $(BIN) $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done
	@echo "all unit tests passed"

clean:
	rm -rf build build-*

-include $(OBJ:.o=.d)
.PHONY: all test clean
