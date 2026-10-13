"""Converts a client DMap file to the dump format of CGameMap::DumpToFile.

    python dmap_to_dump.py <map.DMap> <out.dump> <map id>

The dump feeds sail_sim and the dump tests of map_tests. DMap files are in the
client's map/map folder; ini/GameMap.json maps a map id to its file.

DMap layout: u32 version, u32, char[260] puzzle, i32 width, i32 height, then
height rows of (width x [u16 mask, u16 terrain, i16 altitude] + u32 checksum).
Dump layout: u32 width, u32 height, u32 map id, then per cell u16 mask, i16 altitude.
"""
import struct
import sys


def convert(src, dst, map_id):
    data = open(src, "rb").read()
    offset = 8 + 260
    width, height = struct.unpack_from("<ii", data, offset)
    offset += 8
    blocked = 0
    with open(dst, "wb") as out:
        out.write(struct.pack("<III", width, height, map_id))
        for _ in range(height):
            row = struct.unpack_from("<" + "HHh" * width, data, offset)
            offset += width * 6 + 4
            for x in range(width):
                mask, altitude = row[x * 3], row[x * 3 + 2]
                blocked += mask == 1
                out.write(struct.pack("<Hh", mask, altitude))
    print(f"{width}x{height}, {blocked} of {width * height} tiles blocked")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    convert(sys.argv[1], sys.argv[2], int(sys.argv[3]))
