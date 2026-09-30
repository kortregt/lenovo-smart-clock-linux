# 09 — Home Assistant

The clock shows data from Home Assistant (currently the weather) under the date. It reads
Home Assistant's REST API; nothing needs to be installed in Home Assistant.

- [`ha-poll`](../rootfs-overlay/usr/local/bin/ha-poll) (a shell script, started from
  `/etc/inittab`) fetches a weather entity once a minute with `wget` and `jq`, and writes
  one line such as `76°F  Partly cloudy` to `/run/clock/weather`.
- The clock face reads that file every 5 seconds, and hides the line if the file is
  missing or older than 15 minutes, so it never shows stale weather.

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
