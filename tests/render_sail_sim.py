"""Draws the JSON written by sail_sim as a PNG (standard library only).

    python render_sail_sim.py <sim.json> <out.png> [from second] [to second]

Yellow outline: hunt zone. Green cells: ground swept at the end of the run
(brighter = more recently). Red dots: mob spawns (dim = dead at the end).
Line: the jumps, with a white dot per landing; jumps led by a clump run from
blue at the start of the time window to orange at its end, scouting jumps are
grey. Magenta ticks: Scatter casts with 3+ hits.
"""
import json
import struct
import sys
import zlib

SCALE = 8       # pixels per tile
MARGIN = 6      # tiles around the zone
CELL = 4        # tiles per coverage cell (SailPlanner::kCellSize)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sim = json.load(open(sys.argv[1]))
    t0 = float(sys.argv[3]) * 1000 if len(sys.argv) > 3 else 0
    t1 = float(sys.argv[4]) * 1000 if len(sys.argv) > 4 else float("inf")

    polygon = sim["polygon"]
    min_x = min(p[0] for p in polygon) - MARGIN
    min_y = min(p[1] for p in polygon) - MARGIN
    width = (max(p[0] for p in polygon) + MARGIN - min_x) * SCALE
    height = (max(p[1] for p in polygon) + MARGIN - min_y) * SCALE
    image = bytearray([18, 20, 26] * (width * height))

    def plot(x, y, color, alpha=1.0):
        x, y = int(x), int(y)
        if 0 <= x < width and 0 <= y < height:
            i = (y * width + x) * 3
            for k in range(3):
                image[i + k] = int(image[i + k] * (1 - alpha) + color[k] * alpha)

    def to_px(x, y):
        return (x - min_x) * SCALE, (y - min_y) * SCALE

    def line(a, b, color, thickness=1, alpha=1.0):
        (x0, y0), (x1, y1) = to_px(*a), to_px(*b)
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(steps + 1):
            x = x0 + (x1 - x0) * i / steps
            y = y0 + (y1 - y0) * i / steps
            for dx in range(thickness):
                for dy in range(thickness):
                    plot(x + dx, y + dy, color, alpha)

    def dot(point, radius, color):
        x0, y0 = to_px(*point)
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if dx * dx + dy * dy <= radius * radius:
                    plot(x0 + dx, y0 + dy, color)

    for x, y, staleness in sim["cells"]:
        fresh = 1.0 - staleness
        color = (30 + 20 * fresh, 34 + 70 * fresh, 40 + 20 * fresh)
        x0, y0 = to_px(x - (CELL - 1) / 2, y - (CELL - 1) / 2)
        for dy in range(CELL * SCALE):
            for dx in range(CELL * SCALE):
                plot(x0 + dx, y0 + dy, color)

    for i, vertex in enumerate(polygon):
        line(vertex, polygon[(i + 1) % len(polygon)], (255, 215, 0), 2)
    for x, y, alive in sim["mobs"]:
        dot((x, y), 2, (220, 70, 70) if alive else (110, 60, 60))

    landings = [p for p in sim["landings"] if t0 <= p[2] <= t1]
    for i in range(1, len(landings)):
        f = i / max(1, len(landings) - 1)
        color = (int(80 + 175 * f), int(200 - 60 * f), int(255 - 175 * f))
        if not landings[i][3]:
            color = (150, 150, 165)
        line(landings[i - 1][:2], landings[i][:2], color, 2)
    for landing in landings:
        dot(landing[:2], 2, (255, 255, 255))
    for ox, oy, ax, ay, hits, t in sim["casts"]:
        if t0 <= t <= t1 and hits >= 3:
            line((ox, oy), (ox + (ax - ox) * 0.5, oy + (ay - oy) * 0.5), (255, 120, 255), 1, 0.8)
    if landings:
        dot(landings[0][:2], 5, (80, 255, 120))
        dot(landings[-1][:2], 5, (255, 80, 80))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    rows = b"".join(b"\x00" + bytes(image[y * width * 3:(y + 1) * width * 3]) for y in range(height))
    with open(sys.argv[2], "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n"
                  + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(rows, 6))
                  + chunk(b"IEND", b""))
    print(f"wrote {sys.argv[2]} ({width}x{height}, {len(landings)} landings)")


if __name__ == "__main__":
    main()
