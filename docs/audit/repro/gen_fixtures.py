#!/usr/bin/env python3
"""Synthetic Pickett .cat fixtures for the audit (isolated, scratch only).

Fixed-width SPCAT record: FREQ F13.4, ERR F8.4, LGINT F8.4, DR I2, ELO F10.4,
GUP I3, TAG I7, QNFMT I4, then 12 x I2 quantum numbers (6 upper + 6 lower,
unused slots blank).  QN >= 100 use Pickett's letter code (A0 = 100).
"""
import os, sys

OUT = sys.argv[1]
REPO = sys.argv[2]
os.makedirs(OUT, exist_ok=True)

def qn2(v):
    if v is None:
        return "  "
    if 0 <= v < 100 or -10 < v < 0:
        return "%2d" % v
    if 100 <= v < 360:                      # Pickett letter code
        return chr(ord('A') + v // 10 - 10) + str(v % 10)
    raise ValueError(v)

def line(freq, lgint, qnfmt, up, lo, err=0.0010, dr=3, elo=1.0, gup=11, tag=1):
    ups = [qn2(v) for v in up] + ["  "] * (6 - len(up))
    los = [qn2(v) for v in lo] + ["  "] * (6 - len(lo))
    return ("%13.4f%8.4f%8.4f%2d%10.4f%3d%7d%4d" % (freq, err, lgint, dr, elo, gup, tag, qnfmt)
            + "".join(ups) + "".join(los))

def write(name, rows):
    with open(os.path.join(OUT, name), "w") as f:
        for r in rows:
            f.write(r + "\n")

# 3 QN per state, QNFMT 303 (leading blank in the I4 field)
write("cat3_303.cat", [
    line(3000.1000, -4.0, 303, [5, 1, 5], [4, 1, 4]),
    line(5980.0000, -4.1, 303, [11, 1, 11], [10, 1, 10]),
    line(5999.2867, -3.8649, 303, [11, 0, 11], [10, 1, 9]),     # same as pred.cat:755
    line(7000.0000, -4.2, 303, [25, 3, 22], [24, 3, 21]),
    line(8000.0000, -4.3, 303, [45, 2, 43], [44, 2, 42]),
    line(9000.0000, -4.4, 303, [72, 1, 71], [71, 1, 70]),
])
# 4 QN (asymmetric rotor + one spin: N Ka Kc F), QNFMT 304 (leading blank)
write("cat4_304.cat", [
    line(3100.0000, -4.0, 304, [3, 1, 2, 4], [2, 1, 1, 3]),
    line(3100.3000, -4.1, 304, [3, 1, 2, 3], [2, 1, 1, 2]),     # same first 3 QN, different F
    line(6100.0000, -4.2, 304, [12, 1, 11, 13], [11, 1, 10, 12]),
    line(6100.3000, -4.3, 304, [12, 1, 11, 12], [11, 1, 10, 11]),
])
# 4 QN, vibrational/state number, QNFMT 1404 (no leading blank)
write("cat4_1404.cat", [
    line(2511.3375, -4.0, 1404, [4, 1, 4, 0], [3, 1, 3, 0]),
    line(2511.9000, -4.1, 1404, [4, 1, 4, 1], [3, 1, 3, 1]),    # same first 3 QN, other state
    line(6033.5894, -4.2, 1404, [12, 2, 10, 2], [11, 2, 9, 2]),
    line(6034.0000, -4.3, 1404, [12, 2, 10, 0], [11, 2, 9, 0]),
])
# 5 QN (N Ka Kc F1 F), QNFMT 305
write("cat5_305.cat", [
    line(3200.0000, -4.0, 305, [4, 0, 4, 5, 5], [3, 0, 3, 4, 4]),
    line(3200.3000, -4.1, 305, [4, 0, 4, 5, 6], [3, 0, 3, 4, 5]),
    line(6200.0000, -4.2, 305, [15, 1, 14, 16, 16], [14, 1, 13, 15, 15]),
])
# 6 QN, QNFMT 306
write("cat6_306.cat", [
    line(3300.0000, -4.0, 306, [2, 1, 1, 3, 4, 5], [1, 1, 0, 2, 3, 4]),
    line(3300.3000, -4.1, 306, [2, 1, 1, 3, 4, 4], [1, 1, 0, 2, 3, 3]),
    line(6300.0000, -4.2, 306, [13, 1, 12, 14, 15, 16], [12, 1, 11, 13, 14, 15]),
])
# Pickett letter-coded QN (J = 105 -> "A5")
write("cat_letter.cat", [
    line(3400.0000, -4.0, 303, [105, 3, 102], [104, 3, 101]),
    line(3401.0000, -4.0, 303, [6, 3, 3], [5, 3, 2]),
])
# A real pred.cat excerpt with trailing blanks stripped (common after editing)
with open(os.path.join(REPO, "pred.cat")) as f:
    src = [next(f) for _ in range(5)]
write("cat_trim.cat", [r.rstrip() for r in src])
print("fixtures written to", OUT)
