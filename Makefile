CC := gcc
PKGS := gtk+-3.0 json-glib-1.0
CFLAGS := -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS))

OUT_DIR := out

CASHREG := $(OUT_DIR)/AdavaCashReg
WAREHOUSE := $(OUT_DIR)/AdavaWarehouseManagment

.PHONY: all clean cashreg warehouse

all: cashreg warehouse

out:
	mkdir -p $(OUT_DIR)

cashreg: main.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $(CASHREG) $(LDLIBS)

warehouse: warehouse.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $< -o $(WAREHOUSE) $(LDLIBS)

run: all
	cd $(OUT_DIR) && ./AdavaCashReg

clean:
	rm -rf $(OUT_DIR)
