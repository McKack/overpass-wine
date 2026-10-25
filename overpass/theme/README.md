# OverPass theme

The look of Windows controls (buttons, checkboxes, scroll bars, menus…) in OverPass bottles. It starts
as a copy of Wine's own light theme (`dlls/light.msstyles`, LGPL) and is redrawn to sit well next to
macOS apps. It's our own artwork: don't copy Apple's images or icons.

## Files

| File | What it is |
|---|---|
| `overpass-theme.svg` | The sheet you edit: every image of the theme on one canvas |
| `slices.json` | Where each image sits on the sheet, its states and sizing margins (generated) |
| `make-template.py` | Rebuilds the sheet and `slices.json` from Wine's light theme. Running it again overwrites your edits |
| `split.py` | Cuts the sheet back into the per-image SVGs Wine builds the theme from |

## Editing the sheet

Use Inkscape if you can: it keeps the layers and group ids `split.py` relies on. Other editors work if
they keep both, and keep 1 unit = 1 pixel with the canvas origin where it is.

- Only edit the **Artwork** layer. **Guides** and **Labels** are locked and never end up in the theme.
- Each image is a group named after its file (`blue_button`, `blue_checkbox_26px`…). Don't rename,
  move or ungroup these groups; edit what's inside them.
- Each **magenta frame** is one state of the control, labelled on the right of a family's first size:
  Normal, Hot (mouse over), Pressed, Disabled, and so on. Keep every state inside its frame; anything
  outside is cut off.
- **Dashed cyan lines** are sizing margins. Wine stretches the image to the control's size: the
  middle stretches, the margins don't. Keep rounded corners and borders inside the margins.
- Transparency works and is kept (32-bit images).
- Gradients and filters go in the sheet's `<defs>` as usual; `split.py` copies them into every image.

### Sizes

Families with several sizes (`checkbox` 13px … 52px) are the same control at different display
scales. Wine picks the one matching the bottle's DPI:

| Size | 1st | 2nd | 3rd | 4th | 5th | 6th | 7th |
|---|---|---|---|---|---|---|---|
| Scale | 100% | 125% | 150% | **200%** | 250% | 300% | 400% |

With high-resolution (Retina) mode on, bottles run at 200%, so the 4th size (e.g. `checkbox_26px`) is
the one you'll see most. Without it, the 1st.

### What matters most

The sheet is ordered by how often you'll see a control: buttons, checkboxes, radio buttons, combo
boxes, text fields, scroll bars, spin buttons, tabs, sliders, then menus, list and tree views, toolbars
and tooltips. The `window_*` images at the end are for title bars Wine draws itself, which is rare on
macOS (normal windows get real macOS title bars), so leave them for last.

### Colours that aren't images

Text colours, menu and selection highlight colours and a few fills are numbers in the theme's ini
(in `dlls/light.msstyles/light.rc`), not images. List what you want and they'll be changed there.

## Dark version

A theme can hold several colour schemes. Copy the sheet to `overpass-theme-dark.svg` and redraw it for
dark mode; it becomes the theme's second scheme, used when macOS is in dark mode.

## Building

    overpass/theme/make-template.py          # only to start over from Wine's light theme
    overpass/theme/split.py --out <folder>   # sheet -> one SVG per image

`split.py` writes SVGs in the form Wine's `tools/buildimage` turns into the theme's BMPs (it needs
`rsvg-convert` and ImageMagick). Wiring the result into the build as `overpass.msstyles` is the next
step.
