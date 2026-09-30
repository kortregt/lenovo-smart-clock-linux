#!/usr/bin/env python3
"""
Generate the "ha" weather icon style: Home Assistant's own weather icons, as drawn by its
frontend's weather card (src/data/weather.ts, Apache 2.0), rasterized to PNGs for the clock
face. The frontend builds each icon from a few shared shapes (sun, moon, clouds, rain, snow,
lightning), coloured by CSS variables; this does the same with the default colours.

The paths are read from weather.ts at a pinned commit, in the order they appear in
getWeatherStateSVG(). "exceptional" has no SVG in the frontend; like the frontend, it uses
the Material Design Icons alert-circle-outline icon (Apache 2.0).

Needs rsvg-convert (librsvg2-bin). Run from anywhere:
    python3 userspace/clockface/ha-icons/generate.py
"""
import os
import re
import subprocess
import urllib.request

COMMIT = "a7eb10013206fc7e3dfafde3c79716b7d44f5f72"
URL = f"https://raw.githubusercontent.com/home-assistant/frontend/{COMMIT}/src/data/weather.ts"
SIZES = (56, 96)  # icon under the date, forecast
HERE = os.path.dirname(os.path.abspath(__file__))

# weatherSVGStyles defaults
COLOURS = {
    "rain": 'fill="#30b3ff"',
    "sun": 'fill="#fdd93c"',
    "moon": 'fill="#fcf497"',
    "cloud-back": 'fill="#d4d4d4"',
    "cloud-front": 'fill="#f9f9f9"',
    "snow": 'fill="#f9f9f9" stroke="#d4d4d4" stroke-width="1" paint-order="stroke"',
}

MDI_ALERT_CIRCLE_OUTLINE = (
    "M11,15H13V17H11V15M11,7H13V13H11V7M12,2C6.47,2 2,6.5 2,12A10,10 0 0,0 12,22"
    "A10,10 0 0,0 22,12A10,10 0 0,0 12,2M12,20A8,8 0 0,1 4,12A8,8 0 0,1 12,4"
    "A8,8 0 0,1 20,12A8,8 0 0,1 12,20Z"
)

# The sets from weather.ts
CLOUDY = {"partlycloudy", "cloudy", "fog", "windy", "windy-variant", "hail", "rainy",
          "snowy", "snowy-rainy", "pouring", "lightning", "lightning-rainy"}
RAIN = {"hail", "rainy", "pouring", "lightning-rainy"}
WINDY = {"windy", "windy-variant"}
SNOWY = {"snowy", "snowy-rainy"}
LIGHTNING = {"lightning", "lightning-rainy"}

STATES = ["sunny", "clear-night", "partlycloudy", "partlycloudy-night", "cloudy", "fog",
          "hail", "lightning", "lightning-rainy", "pouring", "rainy", "snowy", "snowy-rainy",
          "windy", "windy-variant"]


def paths_from_weather_ts():
    src = urllib.request.urlopen(URL).read().decode()
    body = src[src.index("const getWeatherStateSVG"):src.index("export const getWeatherStateIcon")]
    paths = re.findall(r'class="([\w-]+)"\s*d="([^"]+)"', body)
    # sunny, clear-night, partly-night moon, partly sun, 2 cloud, 4 rain, 2 pouring,
    # 2 wind, 3 snow, lightning
    assert len(paths) == 18, f"weather.ts changed: {len(paths)} paths"
    return paths


def svg_for(state, p):
    night = state.endswith("-night") and state != "clear-night"
    s = state[: -len("-night")] if night else state
    parts = []
    if s == "sunny":
        parts.append(p[0])
    if s == "clear-night":
        parts.append(p[1])
    if s == "partlycloudy":
        parts.append(p[2] if night else p[3])
    if s in CLOUDY:
        parts += p[4:6]
    if s in RAIN:
        parts += p[6:10]
    if s == "pouring":
        parts += p[10:12]
    if s in WINDY:
        parts += p[12:14]
    if s in SNOWY:
        parts += p[14:17]
    if s in LIGHTNING:
        parts.append(p[17])
    body = "".join(f'<path {COLOURS[cls]} d="{d}"/>' for cls, d in parts)
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 17 17">{body}</svg>'


def main():
    p = paths_from_weather_ts()
    svgs = {state: svg_for(state, p) for state in STATES}
    svgs["exceptional"] = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">'
                           f'<path fill="#d4d4d4" d="{MDI_ALERT_CIRCLE_OUTLINE}"/></svg>')
    for state, svg in svgs.items():
        for size in SIZES:
            out = os.path.join(HERE, f"{state}-{size}.png")
            subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", out],
                           input=svg.encode(), check=True)
    print(f"{len(svgs)} icons x {len(SIZES)} sizes")


if __name__ == "__main__":
    main()
