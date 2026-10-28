"""Builds romfs/cities.tsv and romfs/stations.tsv from radio-browser + GeoNames.

Usage: python build_db.py stations.json cities15000.zip ../romfs timeZones.txt

Stations are attached to the nearest GeoNames city (pop >= 15000) within
MAX_KM; the rest are grouped on a coarse grid and named after their 'state'.
"""
import json, math, sys, zipfile
from collections import defaultdict

MAX_KM = 40
MAX_PER_CITY = 40
MAX_BITRATE = 256  # keep decoding cheap on Old 3DS


def clean(s):
    s = (s or "").replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()
    return " ".join(s.split())


def hav(lat1, lon1, lat2, lon2):
    p = math.pi / 180
    a = (math.sin((lat2 - lat1) * p / 2) ** 2 +
         math.cos(lat1 * p) * math.cos(lat2 * p) * math.sin((lon2 - lon1) * p / 2) ** 2)
    return 12742 * math.asin(math.sqrt(min(1, a)))


TZ = {}  # tz id -> (standard offset min, summer offset min)


def load_tz(path):
    with open(path, encoding="utf-8") as f:
        next(f)
        for line in f:
            c = line.rstrip("\n").split("\t")
            jan, jul = float(c[2]), float(c[3])
            raw = float(c[4])
            # summer offset: whichever of jan/jul differs from raw (south: jan)
            dst = jul if jul != raw else jan
            TZ[c[1]] = (int(round(raw * 60)), int(round(dst * 60)))


def main(stations_path, cities_zip, out_dir):
    stations = json.load(open(stations_path, encoding="utf-8"))

    # GeoNames cities, bucketed on a 1-degree grid for fast nearest lookup
    cities = []
    grid = defaultdict(list)
    with zipfile.ZipFile(cities_zip) as z:
        with z.open("cities15000.txt") as f:
            for line in f:
                c = line.decode("utf-8").rstrip("\n").split("\t")
                lat, lon = float(c[4]), float(c[5])
                idx = len(cities)
                cities.append({"name": c[1], "cc": c[8], "lat": lat, "lon": lon,
                               "pop": int(c[14] or 0), "tz": c[17]})
                grid[(math.floor(lat), math.floor(lon))].append(idx)

    def nearest_city(lat, lon):
        best, bd = None, MAX_KM
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                for i in grid.get((math.floor(lat) + dy, (math.floor(lon) + dx + 180) % 360 - 180), ()):
                    c = cities[i]
                    d = hav(lat, lon, c["lat"], c["lon"])
                    # prefer big cities slightly, so suburbs merge into the metro
                    d_eff = d - min(15, math.log10(max(c["pop"], 1)) * 1.5)
                    if d_eff < bd:
                        best, bd = i, d_eff
        return best

    seen_urls = set()
    groups = defaultdict(list)   # key -> stations
    meta = {}                    # key -> place info
    country_names = {}
    for s in stations:
        url = (s.get("url_resolved") or s.get("url") or "").strip()
        if not url.startswith(("http://", "https://")) or url in seen_urls:
            continue
        if (s.get("codec") or "").upper() != "MP3":
            continue
        br = s.get("bitrate") or 0
        if br > MAX_BITRATE:
            continue
        if not s.get("lastcheckok"):
            continue
        lat, lon = s.get("geo_lat"), s.get("geo_long")
        if lat is None or lon is None or (abs(lat) < 0.01 and abs(lon) < 0.01):
            continue
        name = clean(s.get("name"))
        if not name:
            continue
        seen_urls.add(url)
        cc = (s.get("countrycode") or "").upper()
        if cc and s.get("country"):
            country_names[cc] = clean(s["country"])

        ci = nearest_city(lat, lon)
        if ci is not None:
            key = ("c", ci)
            c = cities[ci]
            meta[key] = (c["name"], c["cc"] or cc, c["lat"], c["lon"], c["tz"])
        else:
            key = ("g", round(lat * 2) / 2, round(lon * 2) / 2)
            if key not in meta:
                meta[key] = (clean(s.get("state")) or clean(s.get("country")) or "?", cc, lat, lon, None)
        tags = clean(s.get("tags"))[:60]
        # http first: cheaper than TLS on the Old 3DS
        prio = (0 if url.startswith("http://") else 1, -(s.get("votes") or 0))
        groups[key].append((prio, name[:60], url, br, tags))

    places = []
    for key, sts in groups.items():
        sts.sort()
        name, cc, lat, lon, tz = meta[key]
        off = TZ.get(tz) if tz else None
        if off is None:
            o = int(round(lon / 15)) * 60
            off = (o, o)
        places.append((name, cc, lat, lon, off, sts[:MAX_PER_CITY]))
    places.sort(key=lambda p: (p[1], p[0]))

    n_st = 0
    with open(f"{out_dir}/cities.tsv", "w", encoding="utf-8", newline="\n") as fc, \
         open(f"{out_dir}/stations.tsv", "w", encoding="utf-8", newline="\n") as fs:
        for name, cc, lat, lon, off, sts in places:
            country = country_names.get(cc, cc)
            fc.write(f"{clean(name)[:40]}\t{country[:32]}\t{lat:.3f}\t{lon:.3f}\t{off[0]}\t{off[1]}\t{n_st}\t{len(sts)}\n")
            for _, sn, url, br, tags in sts:
                fs.write(f"{sn}\t{url}\t{br}\t{tags}\n")
                n_st += 1
    print(f"{len(places)} places, {n_st} stations")


if __name__ == "__main__":
    load_tz(sys.argv[4])
    main(*sys.argv[1:4])
