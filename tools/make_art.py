"""Draws icon.png (48x48), banner.png (256x128) and banner.wav for the .cia.
Standard library only. Usage: python make_art.py <project dir>
"""
import math, random, struct, sys, zlib, wave

ROOT = sys.argv[1] if len(sys.argv) > 1 else ".."
SS = 4  # supersampling


def load_places():
    pts = []
    with open(f"{ROOT}/romfs/cities.tsv", encoding="utf-8") as f:
        for line in f:
            c = line.split("\t")
            pts.append((float(c[2]), float(c[3]), int(c[7])))
    return pts


class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w * SS, h * SS
        self.px = [list(bg) + [255] for _ in range(self.w * self.h)]

    def blend(self, x, y, c, a=1.0):
        if 0 <= x < self.w and 0 <= y < self.h:
            p = self.px[y * self.w + x]
            for i in range(3):
                p[i] = p[i] * (1 - a) + c[i] * a

    def disc(self, cx, cy, r, col_fn, a=1.0):
        cx, cy, r = cx * SS, cy * SS, r * SS
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                dx, dy = (x + .5 - cx) / r, (y + .5 - cy) / r
                d = dx * dx + dy * dy
                if d <= 1:
                    self.blend(x, y, col_fn(dx, dy), a)

    def rect(self, x, y, w, h, c, a=1.0):
        for yy in range(int(y * SS), int((y + h) * SS)):
            for xx in range(int(x * SS), int((x + w) * SS)):
                self.blend(xx, yy, c, a)

    def save_png(self, path):
        w, h = self.w // SS, self.h // SS
        raw = bytearray()
        for y in range(h):
            raw.append(0)
            for x in range(w):
                acc = [0, 0, 0]
                for sy in range(SS):
                    for sx in range(SS):
                        p = self.px[(y * SS + sy) * self.w + x * SS + sx]
                        for i in range(3):
                            acc[i] += p[i]
                raw += bytes(min(255, int(v / (SS * SS))) for v in acc) + b"\xff"

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
        open(path, "wb").write(png)


def stars(cv, n, seed):
    rnd = random.Random(seed)
    for _ in range(n):
        x, y = rnd.randrange(cv.w // SS), rnd.randrange(cv.h // SS)
        b = rnd.randint(110, 255)
        cv.rect(x, y, 1, 1, (b, b, min(255, b + 15)))


def globe(cv, places, cx, cy, R, lat0, lon0, dot):
    d2r = math.pi / 180
    for k, a in ((R + 5, .12), (R + 3, .2), (R + 1.5, .35)):
        cv.disc(cx, cy, k, lambda dx, dy: (80, 150, 255), a)
    cv.disc(cx, cy, R, lambda dx, dy: (int(45 - 30 * (dx + dy) / 2), int(95 - 55 * (dx + dy) / 2),
                                        int(225 - 90 * (dx + dy) / 2)))
    sl0, cl0 = math.sin(lat0 * d2r), math.cos(lat0 * d2r)
    for lat, lon, n in places:
        sl, clt = math.sin(lat * d2r), math.cos(lat * d2r)
        dl = (lon - lon0) * d2r
        x = clt * math.sin(dl)
        y = cl0 * sl - sl0 * clt * math.cos(dl)
        z = sl0 * sl + cl0 * clt * math.cos(dl)
        if z <= 0.05:
            continue
        s = dot * (1.4 if n >= 15 else 1.0)
        cv.rect(cx + x * R - s / 2, cy - y * R - s / 2, s, s, (255, 225, 120), 0.5 + 0.5 * z)


FONT = {  # 5x7
    "R": ["1110", "1001", "1001", "1110", "1010", "1001", "1001"],
    "A": ["0110", "1001", "1001", "1111", "1001", "1001", "1001"],
    "D": ["1110", "1001", "1001", "1001", "1001", "1001", "1110"],
    "I": ["111", "010", "010", "010", "010", "010", "111"],
    "O": ["0110", "1001", "1001", "1001", "1001", "1001", "0110"],
    "3": ["1110", "0001", "0001", "0110", "0001", "0001", "1110"],
    "S": ["0111", "1000", "1000", "0110", "0001", "0001", "1110"],
    " ": ["00", "00", "00", "00", "00", "00", "00"],
}


def text(cv, s, x, y, px, col):
    for ch in s:
        g = FONT[ch]
        for r, row in enumerate(g):
            for c, bit in enumerate(row):
                if bit == "1":
                    cv.rect(x + c * px, y + r * px, px, px, col)
        x += (len(g[0]) + 1) * px


def main():
    places = load_places()

    icon = Canvas(48, 48, (6, 8, 26))
    stars(icon, 25, 1)
    globe(icon, places, 24, 24, 19, -15, -55, 1.1)
    icon.disc(29, 30, 2.6, lambda dx, dy: (235, 40, 40))
    icon.save_png(f"{ROOT}/icon.png")

    ban = Canvas(256, 128, (4, 6, 20))
    stars(ban, 120, 2)
    globe(ban, places, 190, 70, 54, -10, -50, 1.6)
    ban.disc(197, 92, 4, lambda dx, dy: (235, 40, 40))
    text(ban, "RADIO", 14, 34, 5, (255, 255, 255))
    text(ban, "3DS", 14, 78, 5, (255, 170, 40))
    ban.save_png(f"{ROOT}/banner.png")

    # short two-note chime
    rate, out = 22050, bytearray()
    for i in range(int(rate * 1.6)):
        t = i / rate
        v = 0.0
        for start, f in ((0.0, 880.0), (0.25, 1318.5)):
            if t >= start:
                tt = t - start
                v += 0.35 * math.sin(2 * math.pi * f * tt) * math.exp(-tt * 3.2)
        out += struct.pack("<h", int(max(-1, min(1, v)) * 32767))
    with wave.open(f"{ROOT}/banner.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(out))
    print("ok")


main()
