#!/bin/sh
# Fetch the Meteocons (https://meteocons.com, MIT, by Bas Milius) Lottie animations the
# clock face uses, from the npm package on jsDelivr. Pinned: 3.x is still a pre-release.
set -eu
VERSION=3.0.0-next.10
BASE=https://cdn.jsdelivr.net/npm/@meteocons/lottie@$VERSION
cd "$(dirname "$0")"
for name in clear-day clear-night partly-cloudy-day partly-cloudy-night cloudy fog hail \
	    thunderstorms thunderstorms-rain extreme-rain rain snow sleet wind code-orange \
	    not-available; do
	curl -sf -o "$name.json" "$BASE/fill/$name.json"
done
curl -sf -o LICENSE "$BASE/LICENSE"
