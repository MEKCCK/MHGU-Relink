"""Build the static UI font from the text actually present in the mod."""
import argparse
from pathlib import Path
import re

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


def generate(font_path, project, destination):
    # Each port owns a few UI strings. Build the common font from all four.
    projects = [project.parent / name for name in ("android", "port", "eden", "switch")]
    if not all((p / "wheel").is_dir() for p in projects):
        projects = [project]
    paths = []
    for branch in projects:
        paths.extend([branch / "presets/item_names.inc", branch / "wheel/item_names_en.inc"])
        paths.extend((branch / "wheel").glob("*.c"))
        paths.extend(p for p in (branch / "wheel").glob("*.inc") if p.stat().st_size < 200000)
    desired = set(range(32, 127))
    for path in paths:
        literals = re.findall(r'"(?:\\.|[^"\\])*"', path.read_text(encoding="utf-8"))
        desired.update(ord(char) for literal in literals for char in literal if ord(char) >= 32)
    font = TTFont(font_path, recalcTimestamp=False)
    assert desired <= font.getBestCmap().keys(), desired - font.getBestCmap().keys()
    options = subset.Options()
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    sub = subset.Subsetter(options=options)
    sub.populate(unicodes=desired)
    sub.subset(font)
    font = instantiateVariableFont(font, {"wght": 400}, inplace=True)
    names = {1:"GU Wheel UI", 2:"Regular", 3:"GUWheelUI-Regular-041", 4:"GU Wheel UI Regular",
             6:"GUWheelUI-Regular", 16:"GU Wheel UI", 17:"Regular"}
    for rec in font["name"].names:
        if rec.nameID in names:
            rec.string = names[rec.nameID].encode(rec.getEncoding())
    font.save(destination)
    static = TTFont(destination)
    assert desired <= static.getBestCmap().keys() and "fvar" not in static and "glyf" in static
    print(f"Verified {len(desired)} UI and item-name characters.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("font", type=Path)
    parser.add_argument("project", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.font, args.project, args.output)
