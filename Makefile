CC := gcc
PKGS := gtk+-3.0 json-glib-1.0
CFLAGS := -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

OUT_DIR := out

CASHREG := $(OUT_DIR)/AdavaCashReg
WAREMAN := $(OUT_DIR)/AdavaWareMan

.PHONY: all clean cashreg wareman

all: cashreg wareman

out:
	mkdir -p $(OUT_DIR)

cashreg: main.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $(CASHREG) $(LDLIBS)

wareman: wareman.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $(WAREMAN) $(LDLIBS)

run: all
	cd $(OUT_DIR) && ./AdavaCashReg

clean:
	rm -rf $(OUT_DIR)
