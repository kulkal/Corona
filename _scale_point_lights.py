r"""Scale point/spot light intensities in a Corona .map by a factor.

Directional lights (the sun) are left untouched. Idempotent: the first run
snapshots the original to <map>.preboost.bak and every run recomputes from that
snapshot, so re-running with a different factor never compounds.

Usage:  py _scale_point_lights.py <map_path> <factor>
"""
import re
import shutil
import sys

map_path = sys.argv[1] if len(sys.argv) > 1 else r"C:\dev\Corona\assets\maps\CyberpunkAlleyInterp.map"
factor = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
bak = map_path + ".preboost.bak"

import os
if not os.path.exists(bak):
    shutil.copy2(map_path, bak)
    print(f"snapshot -> {bak}")
src = bak  # always recompute from the original

with open(src, "r", encoding="utf-8", errors="replace") as f:
    lines = f.readlines()

in_light = False
cur_type = None
scaled = 0
type_counts = {}
out = []
intensity_re = re.compile(r"^(\s*intensity\s*=\s*)([0-9.eE+-]+)(.*)$")
type_re = re.compile(r'type\s*=\s*"([a-zA-Z]+)"')

for ln in lines:
    if "light" in ln and "=" in ln and "{" in ln:
        in_light = True
        cur_type = None
    if in_light:
        m = type_re.search(ln)
        if m:
            cur_type = m.group(1).lower()
            type_counts[cur_type] = type_counts.get(cur_type, 0) + 1
        im = intensity_re.match(ln)
        if im and cur_type in ("point", "spot"):
            val = float(im.group(2)) * factor
            ln = f"{im.group(1)}{val:g}{im.group(3)}\n"
            scaled += 1
        if ln.strip().startswith("}"):
            in_light = False
            cur_type = None
    out.append(ln)

with open(map_path, "w", encoding="utf-8") as f:
    f.writelines(out)

print(f"factor={factor}  scaled intensities={scaled}  light types seen={type_counts}")
