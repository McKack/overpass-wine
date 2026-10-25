#!/usr/bin/env python3
"""Builds the editable theme sheet from Wine's light theme.

Every image of dlls/light.msstyles becomes one slot on a single SVG sheet, with its states framed and
labelled and its sizing margins marked, and Wine's own artwork in place as a starting point. Edit the
sheet in any SVG editor, then run split.py to turn it back into the per-image SVGs Wine builds from.

    overpass/theme/make-template.py [--out overpass/theme/overpass-theme.svg]
"""

import argparse
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
THEME = REPO / "dlls/light.msstyles"
SVG_NS = "http://www.w3.org/2000/svg"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
SODIPODI_NS = "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
ET.register_namespace("", SVG_NS)
ET.register_namespace("inkscape", INKSCAPE_NS)
ET.register_namespace("sodipodi", SODIPODI_NS)

GAP = 16          # between slots, px
LABEL = 14        # label line height above a slot
FAMILY_GAP = 48   # between families
SHEET_W = 2400    # families are packed left to right in shelves this wide
STATE_LABEL = 70  # room for state names right of a family's first image


PRIORITY = ["button", "checkbox", "radiobutton", "combobox", "edit", "scrollbar", "spin", "tab", "trackbar",
            "groupbox", "menu", "header", "listview", "treeview", "explorer", "toolbar", "tooltip", "status",
            "rebar", "commandlink", "navigation", "taskdialog", "window"]


def theme_ini():
    """The [Class.Part(State)] sections of the theme's ini, as {section: {key: value}}."""
    rc = (THEME / "light.rc").read_text()
    body = rc[rc.index("BLUE_INI TEXTFILE"):]
    body = body[:body.index("\n}\n")]
    sections, current = {}, None
    for line in re.findall(r'^"(.*?)(?:\\r\\n)?"\s*$', body, re.M):
        if m := re.match(r"\[(.*)\]$", line):
            current = sections.setdefault(m.group(1), {})
        elif current is not None and (m := re.match(r"(\w+)\s*=\s*(.*)", line)):
            current[m.group(1)] = m.group(2)
    return sections


def state_names():
    """{"PUSHBUTTON": ["Normal", "Hot", ...]} from include/vsstyle.h's <PART>STATES enums."""
    names = {}
    text = (REPO / "include/vsstyle.h").read_text()
    for enum, body in re.findall(r"enum (\w+)STATES \{(.*?)\};", text, re.S):
        values = re.findall(r"\w+?_(\w+) = (\d+)", body)
        if values:
            ordered = [n for n, v in sorted(values, key=lambda nv: int(nv[1]))]
            # UNCHECKEDNORMAL -> Unchecked Normal
            ordered = [re.sub(r"(?<=[A-Z])(NORMAL|HOT|PRESSED|PUSHED|DISABLED|HOVER|FOCUSED|ALIGN)$", r"_\1", n)
                       for n in ordered]
            names[enum] = [n.replace("_", " ").title() for n in ordered]
    return names


# Parts whose states enum isn't named after the part.
STATE_ENUMS = {
    "ComboBox.DropDownButton": "COMBOBOXSTYLE",
    "Header": "HEADERSTYLE",
    "Navigation.BackButton": "NAV_BACKBUTTON",
    "Navigation.ForwardButton": "NAV_FORWARDBUTTON",
    "Navigation.MenuButton": "NAV_MENUBUTTON",
    "Tooltip.BallonStem": "BALLOONSTEM",
    "Window.SmallCloseButton": "CLOSEBUTTON",
    "Window.MdiCloseButton": "CLOSEBUTTON",
    "Window.MdiMinButton": "MINBUTTON",
    "Window.MdiRestoreButton": "RESTOREBUTTON",
}
STATE_ENUM_PATTERNS = [
    (r"ScrollBar\.(Thumb|LowerTrack|UpperTrack|Gripper)", "SCROLLBARSTYLE"),
    (r"Toolbar\.", "TOOLBARSTYLE"),
    (r"Window\.(Small)?Frame", "FRAME"),
    (r"Window\.(Small)?Caption", "CAPTION"),
]


def image_users(sections):
    """{"blue_button.bmp": (section, image count, sizing margins)} for every image the ini uses."""
    users = {}
    for section, keys in sections.items():
        for key, value in keys.items():
            if re.fullmatch(r"(Glyph)?ImageFile\d*", key):
                count = int(keys.get("ImageCount", "1"))
                if key.startswith("Glyph"):
                    count = int(keys.get("GlyphImageCount", count))
                users.setdefault(value.lower(), (section, count, keys.get("SizingMargins")))
    return users


def states_for(section, count, names):
    section = re.sub(r"\(.*\)", "", section)
    enum = STATE_ENUMS.get(section) or next((e for p, e in STATE_ENUM_PATTERNS if re.match(p, section)), None)
    known = names.get(enum or section.split(".")[-1].upper(), [])
    if count == 1:
        return ["Image"]
    # Window caption buttons hold their states twice: for the active window, then for inactive ones.
    if section.startswith("Window.") and count == 2 * len(known):
        return known + [f"{n} (Inactive)" for n in known]
    return [known[i] if i < len(known) else f"State {i + 1}" for i in range(count)]


def num(value):
    return float(re.sub(r"[a-z]+$", "", value))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "overpass-theme.svg")
    args = parser.parse_args()

    sections, names = theme_ini(), state_names()
    users = image_users(sections)

    # Families: the sizes of one image (blue_checkbox_13px ... _52px) side by side. The controls seen
    # most come first on the sheet.
    def priority(path):
        name = path.stem.removeprefix("blue_")
        return next((i for i, p in enumerate(PRIORITY) if name.startswith(p)), len(PRIORITY)), name

    families = {}
    for svg in sorted(THEME.glob("*.svg"), key=priority):
        family = re.sub(r"_\d+px$", "", svg.stem)
        families.setdefault(family, []).append(svg)
    for sizes in families.values():
        sizes.sort(key=lambda p: int(m.group(1)) if (m := re.search(r"_(\d+)px$", p.stem)) else 0)

    # Each family is a block: its sizes side by side. Blocks are packed into shelves.
    def block_width(family, files):
        images = sum(num(ET.parse(f).getroot().get("width")) + GAP for f in files) + STATE_LABEL
        return max(images, len(family.removeprefix("blue_")) * 7.5)  # room for the title

    art, guides, labels, slices = [], [], [], []
    shelf_x, shelf_y, shelf_h = 0.0, LABEL * 2, 0.0
    for family, files in families.items():
        bw = block_width(family, files)
        if shelf_x and shelf_x + bw > SHEET_W:
            shelf_x, shelf_y, shelf_h = 0.0, shelf_y + shelf_h + FAMILY_GAP + LABEL * 2, 0.0
        x, y = shelf_x, shelf_y
        labels.append(f'<text x="{x:g}" y="{y - LABEL - 2:g}" class="family">{family.removeprefix("blue_")}</text>')
        for svg in files:
            root = ET.parse(svg).getroot()
            width, height = num(root.get("width")), num(root.get("height"))
            depth = int(root.get("id").split("-")[1])
            vb = [float(v) for v in root.get("viewBox").split()]
            section, count, margins = users.get(svg.stem.lower() + ".bmp", (None, 1, None))
            states = states_for(section, count, names) if section else ["(unused)"]

            # Wine's artwork, scaled from its own viewBox to 1 unit = 1 pixel at the slot, the way SVG's
            # default preserveAspectRatio (xMidYMid meet) draws it: uniformly, centred.
            scale = min(width / vb[2], height / vb[3])
            ox, oy = x + (width - vb[2] * scale) / 2, y + (height - vb[3] * scale) / 2
            group = ET.Element(f"{{{SVG_NS}}}g", {
                "id": svg.stem,
                f"{{{INKSCAPE_NS}}}label": svg.stem,
                "transform": f"translate({ox:.6g} {oy:.6g}) scale({scale:.8g}) translate({-vb[0]:g} {-vb[1]:g})",
            })
            for child in root:
                if child.tag.endswith("metadata"):
                    continue
                for element in child.iter():
                    if "id" in element.attrib:  # keep ids unique across the sheet
                        element.set("id", f"{svg.stem}-{element.get('id')}")
                group.append(child)
            art.append(ET.tostring(group, encoding="unicode"))

            # Frames, state names and sizing margins.
            state_h = height / len(states)
            size = re.search(r"_(\d+px)$", svg.stem)
            caption = size.group(1) if size else f"{width:g}×{state_h:g}"
            labels.append(f'<text x="{x:g}" y="{y - 3:g}" class="slot">{caption}</text>')
            m = [int(v) for v in margins.split(",")] if margins else [0, 0, 0, 0]
            for i, state in enumerate(states):
                top = y + i * state_h
                guides.append(f'<rect x="{x:g}" y="{top:g}" width="{width:g}" height="{state_h:g}" class="frame"/>')
                if any(m):  # left, right, top, bottom: the parts that don't stretch
                    l, r, t, b = m
                    guides.append(f'<path d="M{x + l:g} {top:g}v{state_h:g}M{x + width - r:g} {top:g}v{state_h:g}'
                                  f'M{x:g} {top + t:g}h{width:g}M{x:g} {top + state_h - b:g}h{width:g}" class="margin"/>')
                if svg is files[0]:
                    size = max(3, min(7, state_h - 1))
                    labels.append(f'<text x="{x + width + 3:g}" y="{top + size:g}" class="state" '
                                  f'style="font-size:{size:g}px">{state}</text>')
            slices.append({"file": svg.name, "x": x, "y": y, "width": width, "height": height, "depth": depth,
                           "section": section, "states": states, "sizingMargins": m})
            x += width + GAP + (STATE_LABEL if svg is files[0] else 0)
            shelf_h = max(shelf_h, height)
        shelf_x += bw + FAMILY_GAP
    y = shelf_y + shelf_h + FAMILY_GAP

    sheet_w, sheet_h = SHEET_W, y
    args.out.write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<!-- OverPass theme sheet. Edit the shapes on the "Artwork" layer only, keep each image inside its
     magenta frames, and don't rename or move the image groups: split.py cuts the sheet at these exact
     positions. Dashed cyan lines are the sizing margins: the parts outside them keep their size when
     Wine stretches the image, the middle stretches. Generated by make-template.py. -->
<svg xmlns="{SVG_NS}" xmlns:inkscape="{INKSCAPE_NS}" xmlns:sodipodi="{SODIPODI_NS}"
     width="{sheet_w:g}" height="{sheet_h:g}" viewBox="0 0 {sheet_w:g} {sheet_h:g}">
 <style>
  .family {{ font: bold 12px -apple-system, Helvetica, sans-serif; fill: #333 }}
  .slot {{ font: 9px -apple-system, Helvetica, sans-serif; fill: #777 }}
  .state {{ font: 7px -apple-system, Helvetica, sans-serif; fill: #b0b }}
  .frame {{ fill: none; stroke: #f0f; stroke-width: .25 }}
  .margin {{ fill: none; stroke: #0bd; stroke-width: .25; stroke-dasharray: 1 1 }}
 </style>
 <rect width="100%" height="100%" fill="#f4f4f4" sodipodi:insensitive="true"/>
 <g id="guides" inkscape:groupmode="layer" inkscape:label="Guides" sodipodi:insensitive="true">
  {chr(10).join("  " + g for g in guides)}
 </g>
 <g id="labels" inkscape:groupmode="layer" inkscape:label="Labels" sodipodi:insensitive="true">
  {chr(10).join("  " + l for l in labels)}
 </g>
 <g id="artwork" inkscape:groupmode="layer" inkscape:label="Artwork">
  {chr(10).join("  " + a for a in art)}
 </g>
</svg>
''')
    (args.out.with_name("slices.json")).write_text(json.dumps(slices, indent=1))
    print(f"{args.out}: {len(slices)} images in {len(families)} families, {sheet_w:g}×{sheet_h:g}px")


if __name__ == "__main__":
    main()
