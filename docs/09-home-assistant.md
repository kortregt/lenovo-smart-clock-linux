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

The icons are Home Assistant's own weather icons from
[Material Design Icons](https://pictogrammers.com/library/mdi/) (Apache 2.0): a 4 KB subset
of the font, [`userspace/clockface/fonts/mdi-weather.ttf`](../userspace/clockface/fonts/),
built into the clock face binary. To add icons, subset the full font again with
`python3 -m fontTools.subset materialdesignicons-webfont.ttf --unicodes=U+F0599,...`.

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
