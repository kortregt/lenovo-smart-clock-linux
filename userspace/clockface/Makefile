# Cross-build the clock face as a static binary (runs on Alpine's musl).
#   make LVGL=/path/to/lvgl          (LVGL v9.6: git clone -b v9.6.0 https://github.com/lvgl/lvgl)
LVGL    ?= $(HOME)/build/lvgl
CC      := aarch64-linux-gnu-gcc
CXX     := aarch64-linux-gnu-g++
AR      := aarch64-linux-gnu-ar
OUT     ?= build
CFLAGS  := -O2 -Wall -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL)
LDFLAGS := -static -lm

CXXFLAGS := $(CFLAGS) -std=c++17

# LVGL, including the ThorVG vector library (C++) for Lottie
LVGL_SRCS := $(shell find $(LVGL)/src -type f -name '*.c')
LVGL_CXX_SRCS := $(shell find $(LVGL)/src -type f -name '*.cpp')
LVGL_OBJS := $(patsubst $(LVGL)/%.c,$(OUT)/lvgl/%.o,$(LVGL_SRCS)) \
	     $(patsubst $(LVGL)/%.cpp,$(OUT)/lvgl/%.o,$(LVGL_CXX_SRCS))

$(OUT)/clockface: $(OUT)/main.o $(OUT)/liblvgl.a
	$(CXX) -o $@ $^ $(LDFLAGS)

$(OUT)/main.o: main.c lv_conf.h $(wildcard meteocons/*.json)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/liblvgl.a: $(LVGL_OBJS)
	$(AR) rcs $@ $^

$(OUT)/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -w -c -o $@ $<

$(OUT)/lvgl/%.o: $(LVGL)/%.cpp lv_conf.h
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -w -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: clean
