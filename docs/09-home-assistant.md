# 09 — Home Assistant

The clock shows data from Home Assistant (currently the weather) under the date. It reads
Home Assistant's REST API; nothing needs to be installed in Home Assistant.

- [`ha-poll`](../rootfs-overlay/usr/local/bin/ha-poll) (a shell script, started from
  `/etc/inittab`) fetches a weather entity once a minute with `wget` and `jq`, and writes
  two lines to `/run/clock/weather`: the condition (`partlycloudy`, or
  `partlycloudy-night` when `sun.sun` is below the horizon) and the text
  (`76°F  Partly cloudy`).
- The clock face reads that file every 5 seconds and shows the condition as an icon next
  to the text. It hides both if the file is missing or older than 15 minutes, so it never
  shows stale weather.

- Every 10 minutes `ha-poll` also calls the `weather.get_forecasts` service (daily) and
  writes `/run/clock/forecast`, one day per line: `condition|day|high|low`. Tapping the
  screen shows the next five days for 15 seconds (tap again to close it).

### Icons

Two styles, chosen with `ICON_STYLE` in
[`/etc/clock/clockface.conf`](../rootfs-overlay/etc/clock/clockface.conf):

- **`ha` (default):** Home Assistant's own weather card icons, so the clock matches the
  dashboard. The frontend draws them from a few shared shapes (sun, moon, clouds, rain,
  snow, lightning); [`ha-icons/generate.py`](../userspace/clockface/ha-icons/generate.py)
  rebuilds them the same way from the frontend's `weather.ts` (Apache 2.0, pinned commit)
  and renders PNGs at the two sizes used, which are committed and built into the binary.
  Static, so they cost nothing to show.
- **`meteocons`:** [Meteocons](https://meteocons.com) by Bas Milius (MIT), animated
  full-colour Lottie icons, played with LVGL's Lottie player (ThorVG). They're in
  [`meteocons/`](../userspace/clockface/meteocons/), fetched by `fetch.sh` (pinned, since 3.x
  is a pre-release). Each frame is rendered in software:

  | | CPU (one core) |
  |---|---|
  | Icon under the date, animated all the time (`ICON_ANIMATION=always`) | ~22% |
  | One play-through a minute (`minute`, default) | ~0% between minutes |
  | Forecast shown (five icons, for 15 seconds) | ~43% |

## Setup

1. In Home Assistant: your profile → **Security** → **Long-lived access tokens** →
   create one.
2. On the clock, create `/etc/clock/ha.conf` (see
   [`ha.conf.example`](../rootfs-overlay/etc/clock/ha.conf.example)) and keep it private:

   ```sh
   cp /etc/clock/ha.conf.example /etc/clock/ha.conf
   chmod 600 /etc/clock/ha.conf
   vi /etc/clock/ha.conf     # HA_URL, HA_TOKEN, HA_WEATHER
   kill $(pidof ha-poll)     # init restarts it with the new settings
   ```

`HA_WEATHER` is any `weather.*` entity; a new Home Assistant install creates
`weather.forecast_home` (Met.no). The clock must be able to reach Home Assistant; check with
`wget -O - $HA_URL/api/`, which should answer `401 Unauthorized` without a token.
