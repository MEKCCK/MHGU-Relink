"""Export the matching GU support-action names, HUD sprite IDs and color indices."""
import argparse
import json
import struct
import zlib
from pathlib import Path


def entry(source, target):
    with source.open("rb") as fp:
        header = fp.read(12)
        assert header[:4] == b"ARC\0"
        table = fp.read(80 * struct.unpack_from("<H", header, 6)[0])
        for offset in range(0, len(table), 80):
            name = table[offset:offset + 64].partition(b"\0")[0].decode()
            if name != target:
                continue
            length, total, start = struct.unpack_from("<III", table, offset + 68)
            fp.seek(start)
            data = fp.read(length)
            size = total & 0x1fffffff
            decoded = data if length == size else zlib.decompress(data)
            assert len(decoded) == size
            return decoded
    raise ValueError(f"{target} not found in {source}")


def extract(native):
    name = r"otomo\support\spt_act_base"
    records = entry(native / "loc/arc/resident.arc", name)
    count, = struct.unpack_from("<I", records, 4)
    assert count == 58 and len(records) == 8 + 24 * count
    names = {}
    for lang in ("eng", "chS"):
        gmd = entry(native / lang / f"arc/resident_{lang}.arc", f"{lang}\\{name}_{lang}")
        assert gmd[:4] == b"GMD\0"
        length, = struct.unpack_from("<I", gmd, 32)
        messages = gmd[-length:].decode("utf-8").split("\0")
        assert len(messages) == 2 * count + 1 and messages[-1] == ""
        names[lang] = messages[:-1:2]

    rows = []
    for action in range(count):
        record = records[8 + action * 24:32 + action * 24]
        assert struct.unpack_from("<I", record)[0] == action
        icon, color = record[18:20]
        assert icon < 100 and color <= 16
        rows.append((names["chS"][action], names["eng"][action], icon, color))
    assert rows[1] == ("药草笛之技", "Herb Horn", 34, 9)
    assert rows[46][1] == "Release the Beast" and rows[57][1] == "Cage the Beast"
    return rows


def generate(rows):
    lines = [
        "/* Generated from nativeNX resident ARC support-action records and localized GMDs. */",
        "static const struct {",
        "    const char *zh, *en;",
        "    unsigned char icon, color;",
        "} prowler_action_art[] = {",
    ]
    for zh, en, icon, color in rows:
        lines.append("    {%s, %s, %d, %d}," % (
            json.dumps(zh, ensure_ascii=False), json.dumps(en, ensure_ascii=False), icon, color))
    lines.extend([
        "};",
        "",
        "static const char *prowler_action_name(unsigned id, int use_english){",
        "    if(!id || id >= sizeof(prowler_action_art)/sizeof(prowler_action_art[0]))return \"\";",
        "    return use_english ? prowler_action_art[id].en : prowler_action_art[id].zh;",
        "}",
        "",
        "static void draw_prowler_action(ImDrawList *d, float x, float y, unsigned id){",
        "    if(!id || id >= sizeof(prowler_action_art)/sizeof(prowler_action_art[0]))return;",
        "    draw_packed_sprite(d, &prowler_icons[prowler_action_art[id].icon], x, y, 32, 96,",
        "                       item_palette[prowler_action_art[id].color]);",
        "}",
        "",
    ])
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("nativeNX", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(generate(extract(args.nativeNX)), encoding="utf-8", newline="\n")
