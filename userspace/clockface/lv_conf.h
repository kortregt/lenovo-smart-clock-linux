/*
 * LVGL configuration for the clock face. Only what differs from LVGL's defaults
 * (lv_conf_internal.h fills in the rest).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_STDLIB_MALLOC	LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING	LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF	LV_STDLIB_CLIB

#define LV_USE_OS		LV_OS_NONE
#define LV_COLOR_FORMAT_DEFAULT	LV_COLOR_FORMAT_XRGB8888
#define LV_DEF_REFR_PERIOD	33

/* Fonts: TrueType files at any size, loaded from memory */
#define LV_USE_TINY_TTF		1
#define LV_TINY_TTF_FILE_SUPPORT 0

#define LV_USE_LOG		1
#define LV_LOG_LEVEL		LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF		1

#define LV_BUILD_EXAMPLES	0
#define LV_BUILD_DEMOS		0

#endif /* LV_CONF_H */
