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

/* Lottie (the Meteocons weather animations): ThorVG, which needs float and matrices */
#define LV_USE_FLOAT		1
#define LV_USE_MATRIX		1
#define LV_USE_VECTOR_GRAPHIC	1
#define LV_USE_THORVG		1
#define LV_USE_THORVG_INTERNAL	1
#define LV_USE_LOTTIE		1

/* LVGL's built-in font, for its symbols (the volume bar's speaker) */
#define LV_FONT_MONTSERRAT_28	1

/* PNG decoding (the "ha" weather icons) */
#define LV_USE_LODEPNG		1

#define LV_USE_LOG		1
#define LV_LOG_LEVEL		LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF		1

#define LV_BUILD_EXAMPLES	0
#define LV_BUILD_DEMOS		0

#endif /* LV_CONF_H */
