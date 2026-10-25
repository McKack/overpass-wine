#!/usr/bin/env python3
"""Cuts the theme sheet back into the per-image SVGs Wine builds a theme from.

Each image group on the sheet's Artwork layer (found by its id, e.g. blue_button) becomes <id>.svg, cut
at the position and size make-template.py recorded in slices.json, in the form Wine's tools/buildimage
turns into a BMP. The sheet's <defs> (gradients, filters) go into every file.

    overpass/theme/split.py --out OUT_DIR [--sheet overpass/theme/overpass-theme.svg]
"""

import argparse
import copy
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--sheet", type=Path, default=HERE / "overpass-theme.svg")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    sheet = ET.parse(args.sheet).getroot()
    groups = {g.get("id"): g for g in sheet.iter(f"{{{SVG_NS}}}g") if g.get("id")}
    parents = {child: parent for parent in sheet.iter() for child in parent}
    defs = list(sheet.iter(f"{{{SVG_NS}}}defs"))
    args.out.mkdir(parents=True, exist_ok=True)

    missing = []
    for s in json.loads((HERE / "slices.json").read_text()):
        name = Path(s["file"]).stem
        group = groups.get(name)
        if group is None:
            missing.append(name)
            continue
        w, h = s["width"], s["height"]
        svg = ET.Element(f"{{{SVG_NS}}}svg", {
            "id": f"bitmap:{w:g}-{s['depth']}",
            "width": f"{w:g}", "height": f"{h:g}",
            "viewBox": f"{s['x']:g} {s['y']:g} {w:g} {h:g}",
            "version": "1.1",
        })
        svg.extend(copy.deepcopy(d) for d in defs)
        # Keep transforms an editor may have put on the layers around the group.
        holder, ancestor = svg, parents.get(group)
        wrappers = []
        while ancestor is not None and ancestor is not sheet:
            if ancestor.get("transform"):
                wrappers.insert(0, ET.Element(f"{{{SVG_NS}}}g", {"transform": ancestor.get("transform")}))
            ancestor = parents.get(ancestor)
        for wrapper in wrappers:
            holder.append(wrapper)
            holder = wrapper
        holder.append(copy.deepcopy(group))
        ET.ElementTree(svg).write(args.out / s["file"], encoding="UTF-8", xml_declaration=True)

    if missing:
        sys.exit(f"the sheet has no group for: {', '.join(missing)}")


if __name__ == "__main__":
    main()
