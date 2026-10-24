# Balloon flight 2026-10-24 (16:00-18:30 Berlin, 14:00-16:30 UTC)

Exported from InfluxDB (bucket `telemetry`, node 1, receiver gs1). 1237 packets.

- `flight_clean.csv`: analysis-ready, one row per received packet (use this).
- `flight_raw.csv`: pivoted dump of every InfluxDB field, unmodified.

## Read before analysing
- **Receiver position**: `ground_distance_km`, `slant_range_km` and `elevation_deg` in `flight_clean.csv` are
  computed (with `bridge/geo.py`) from the tracker's first GPS fix at 16:00:02 (52.2727027 N, 9.5850285 E, 86 m),
  taken as the launch / receiver position. The original `distance_km`, `slant_range_km` and `elevation_deg`
  in `flight_raw.csv` used the hard-coded Berlin coordinates in `config.h` and are **wrong**. The receiver
  was stationary at the launch site, so this is the correct geometry (altitudes are GPS altitudes).
- `temp_c_PLACEHOLDER` is a constant 21 C (no real temperature sensor yet).
- Data has gaps (receiver out of range / low SNR), mostly 16:50-18:06, up to ~8.6 min. Only packets that
  reached the receiver are here; the tracker transmitted every ~2 s.
- Data ends at 18:23:14 (alt 9886 m) with no packets after that: this is the last *received* packet, not
  necessarily burst/landing.
- Times: `time_utc` is the receiver clock (laptop); `time_berlin` is CEST (UTC+2).

## Travelled distance, speed, heading (added columns in `flight_clean.csv`)
- `path_ground_km` / `path_3d_km`: cumulative distance along the GPS track (horizontal / including altitude).
  `path_vs_crow_ratio` = path / straight-line distance from launch. Final: 175.9 km along the track vs 133.2 km
  straight-line (ratio 1.32). Underestimates slightly across data gaps (straight line between received points);
  GPS jitter on the ground adds a little.
- `ground_speed_kmh`, `heading_deg`: over a ~60 s trailing window (blank if the window is under 20 s or the
  movement under 10 m). `heading_deg` is the direction the balloon moved TOWARD (0 = north, 90 = east).
- Wind shift: below about 7 km the balloon moved toward ~55-80 deg (ENE) at 20-57 km/h; above 7 km it turned
  toward ~145-157 deg (SSE) and sped up to ~160-210 km/h (max 210 km/h at 9.4 km, 18:16:17), consistent with a
  jet-stream layer.

## 3D replay
`flight_3d_replay.html`: self-contained animation (no internet or libraries needed; double-click to open in a
browser). Drag to rotate, wheel/pinch to zoom, click the timeline to scrub, space = play/pause. The balloon label shows altitude and ground speed. Playback speed: -/+ buttons, the Speed dropdown (1x real time to 1200x), or the Up/Down arrow keys. Colour the track
by modelled air temperature (default), altitude, ground speed, RSSI or battery; the HUD shows the modelled air
temperature and the timeline has a temperature trace; dashed segments and the shaded timeline bands mark periods with no
packets (position is interpolated there). Height exaggeration (default 6x) is visual only.

## Air temperature columns (modelled, not measured)
- `air_temp_model_c`: outside air temperature at the tracker's altitude, from the Open-Meteo weather model
  (<https://open-meteo.com>, fetched 2026-10-24). Method: hourly surface + pressure-level (1000-250 hPa) temperatures
  at six points along the real track (every 30 min); each packet is interpolated in time, in position (between the
  two bracketing points) and in height (by geopotential height). 9 of 1237 rows fall just outside a profile's height
  range and use the nearest segment's slope.
- `air_temp_isa_from_16h_c`: simple estimate: 10.2 C at the 16:00 surface (Wennigsen, 82 m) minus 6.5 C per km. It
  runs 0-2.5 C colder than the model up to 6 km and about 5 C colder at the top.
- Neither is a measurement. The tracker's own `temp_c_PLACEHOLDER` is a constant 21 C (no real sensor). Model
  values are typically good to a few degrees; they smooth small-scale layers and the stratosphere edge.
- Model range over the flight: 10.2 C at launch down to about -48 C at the last packet (9,886 m).
- Map underlay: 56 OpenStreetMap tiles (zoom 10, about 94 m/pixel, covering +-80 km around the track) are embedded
  in the file as images and textured onto the ground plane, so it still works offline (page is about 2.7 MB).
  Toggle and brightness are in the options panel. Tiles were fetched once through the project's `tilecache`
  proxy. Map (c) OpenStreetMap contributors (shown on the page).
