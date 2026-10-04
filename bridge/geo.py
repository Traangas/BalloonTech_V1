"""Ground-to-air link geometry: great-circle distance, slant range, elevation angle."""
from __future__ import annotations

import math

EARTH_RADIUS_M = 6371000.0


def haversine_distance_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2) ** 2
    return 2 * EARTH_RADIUS_M * math.asin(math.sqrt(a))


def slant_range_m(ground_distance_m: float, alt_diff_m: float) -> float:
    return math.hypot(ground_distance_m, alt_diff_m)


def elevation_angle_deg(ground_distance_m: float, alt_diff_m: float) -> float:
    if ground_distance_m == 0:
        if alt_diff_m == 0:
            return 0.0
        return 90.0 if alt_diff_m > 0 else -90.0
    return math.degrees(math.atan2(alt_diff_m, ground_distance_m))
