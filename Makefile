CC := gcc
CFLAGS := -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0)
LDLIBS := $(shell pkg-config --libs gtk+-3.0)

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
