# Cross-build the clock face as a static binary (runs on Alpine's musl).
#   make LVGL=/path/to/lvgl          (LVGL v9.6: git clone -b v9.6.0 https://github.com/lvgl/lvgl)
LVGL    ?= $(HOME)/build/lvgl
CC      := aarch64-linux-gnu-gcc
AR      := aarch64-linux-gnu-ar
OUT     ?= build
CFLAGS  := -O2 -Wall -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL)
LDFLAGS := -static -lm

LVGL_SRCS := $(shell find $(LVGL)/src -type f -name '*.c')
LVGL_OBJS := $(patsubst $(LVGL)/%.c,$(OUT)/lvgl/%.o,$(LVGL_SRCS))

$(OUT)/clockface: $(OUT)/main.o $(OUT)/liblvgl.a
	$(CC) -o $@ $^ $(LDFLAGS)

$(OUT)/main.o: main.c lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/liblvgl.a: $(LVGL_OBJS)
	$(AR) rcs $@ $^

$(OUT)/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: clean
