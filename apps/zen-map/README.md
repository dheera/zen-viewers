# Zen Map

Open a **GPX, KML, or GeoJSON** file and add a live OpenStreetMap basemap when you want context. Rounded borderless windows, hover controls, and pointer-anchored zoom keep the map in focus.

```sh
zen-map walk.gpx
zen-map places.geojson
```

- **Drag** to pan. **Scroll** or use **+/−** to zoom.
- **F / Home** fits the file. **Arrow keys** pan.
- **M / Map** toggles live tiles. Open another file with **Ctrl+O** or drop it in the window.
- **F11** toggles fullscreen. Drag the top edge to move the window.

Zen Map reads GPX tracks, routes and waypoints, KML placemark geometry, and standard GeoJSON geometries. Coordinates use WGS84 longitude and latitude. Named points are labeled, polygons retain holes, and routes crossing the antimeridian remain contiguous. Parsing runs in a cancellable worker and accepts files up to 64 MiB with up to two million coordinates.

## Live tiles and privacy

Local overlays work offline. Choose **Map** or press **M** to connect. The tile provider then sees your IP address and the approximate area being viewed. Opening another file turns the basemap off again.

The default provider is `https://tile.openstreetmap.org/{z}/{x}/{y}.png`. Zen Map sends its own User-Agent and displays attribution while the basemap is enabled. It requests visible tiles from the center outward after navigation settles, uses at most four concurrent requests, and cancels obsolete requests. Once visible tiles finish, it can warm up to four directly adjacent tiles at the same zoom, favoring the direction of movement. This uses one idle request at a time and stays within one tile of the viewport. No other zoom levels or offline areas are downloaded. The [OpenStreetMap tile usage policy](https://operations.osmfoundation.org/policies/tiles/) permits this modest short-range lookahead during interactive viewing.

The persistent cache under `$XDG_CACHE_HOME/zen-viewers/maps` (normally `~/.cache/zen-viewers/maps`) honors Cache-Control and Expires, with ETag and Last-Modified revalidation. Tiles without freshness headers use a seven-day fallback. Sessions share the disk cache and use a bounded in-memory cache. Remove that directory to clear the disk cache.

To switch providers, set an HTTPS tile URL and the provider's attribution and credit link. Localhost test servers can use HTTP.

```sh
ZEN_MAP_TILE_URL='https://tiles.example.org/{z}/{x}/{y}.png' \
ZEN_MAP_TILE_ATTRIBUTION='© Your tile provider' \
ZEN_MAP_TILE_CREDIT_URL='https://tiles.example.org/credits' zen-map walk.gpx
```

Tiles must be 256 × 256 PNGs in Web Mercator XYZ order. The GPU composites tile textures during interaction. Zooming reprojects cached coarser or finer tiles until exact detail arrives. Panning retains the geographical overlap and can reveal warmed neighbors immediately. Areas that have never been fetched still need a network response. Networking, file I/O and decoding run on workers.

## Build

Run `make zen-map` from the repository root. On Ubuntu, install `libgtk-4-dev libcurl4-gnutls-dev libxml2-dev libjson-glib-dev`.

Parser regression checks live in `tests/test-data.c`. The UI and cache smoke tests use a local HTTP tile server. `tests/continuity-smoke.py` blocks new tile responses while checking cropped and reprojected pixels during zoom and pan, then verifies refinement, lookahead bounds, ordering, and concurrency. It never contacts OpenStreetMap.

```sh
xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session -- \
  python3 apps/zen-map/tests/continuity-smoke.py build/zen-map
```
