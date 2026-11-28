"""Pack Prowler art from decoded HD Chinese quest and resident GUI atlases."""
import argparse
import re
import struct
from pathlib import Path
from PIL import Image


def frames(hud):
    if hud.size != (2048, 2048):
        raise ValueError("Expected the 2048px quest_chS HUD")
    normal = hud.crop((768, 1216, 1024, 1472))
    # Discard the one-row bleed from the neighbouring HUD strip.
    normal.paste(normal.crop((0, 1, 256, 2)), (0, 0))
    selected = Image.new("RGBA", (380, 320))
    selected.alpha_composite(hud.crop((480, 1216, 768, 1504)), (46, 16))
    selected.alpha_composite(hud.crop((1186, 1728, 1566, 2048)))
    return normal, selected


def pack(hud, icons, actions, output):
    if icons.size != (1024, 2048):
        raise ValueError("Expected the 1024x2048 resident_chS icon atlas")
    used = sorted({int(n) for n in re.findall(r'", (\d+), \d+\},', actions)})
    if not used or max(used) >= 100:
        raise ValueError("Invalid support action icon table")
    pages = list(frames(hud))
    sprites = []
    for start in range(0, len(used), 10):
        group = used[start:start + 10]
        sheet = Image.new("RGBA", (490, 98*((len(group)+4)//5)))
        for j, icon in enumerate(group):
            x, y = (icon%10)*96, 832+(icon//10)*96
            u, v = 1+j%5*98, 1+j//5*98
            sheet.paste(icons.crop((x, y, x+96, y+96)), (u,v))
            sprites.append(f"    [{icon}]={{{u},{v},{20+len(pages)},0,0,96,96}},")
        pages.append(sheet)
    lines = []
    for i, page in enumerate(pages):
        assert page.width*page.height*4 <= 512*1024
        lines.append(f"static const unsigned prowler_pixels_{i}[]={{")
        words = [str(v) for v, in struct.iter_unpack("<I",page.tobytes())]
        lines.extend(",".join(words[j:j+32])+"," for j in range(0,len(words),32))
        lines.append("};")
    lines += ["static const WheelSprite prowler_frame_normal={0,0,20,0,0,256,256};",
              "static const WheelSprite prowler_frame_selected={0,0,21,0,0,380,320};",
              "static const WheelSprite prowler_icons[100]={", *sprites, "};"]
    lines.append("#define PROWLER_ATLAS_PAGES " + ",".join(
        f"{{{p.width},{p.height},prowler_pixels_{i}}}" for i,p in enumerate(pages)))
    output.write_text("\n".join(lines)+"\n",encoding="ascii",newline="\n")


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    for arg in ("hud", "icons", "action_table", "output"):
        cli.add_argument(arg,type=Path)
    args = cli.parse_args()
    pack(Image.open(args.hud).convert("RGBA"), Image.open(args.icons).convert("RGBA"),
         args.action_table.read_text(encoding="utf-8"), args.output)
