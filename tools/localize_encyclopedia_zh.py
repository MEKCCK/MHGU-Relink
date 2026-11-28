#!/usr/bin/env python3
"""Rewrite the embedded encyclopedia's Chinese details with Chinese stat labels.

The records were extracted from MHGU Database 2.4.5 (mhgu.db): monsters, items, quests,
weapons, armor families, Palico gear, skills, decorations, Horn melodies and locations.
The database has Chinese names/goals but English stat labels and monster states, so the
Chinese detail text still mixed both. This tool reads runtime/encyclopedia_data.h, applies
a fixed label dictionary to the Chinese strings only, rebuilds the table (offsets, string
pool, icons) and writes the header back. It is deterministic: the same input gives the
same bytes.

    python3 tools/localize_encyclopedia_zh.py
    python3 tools/localize_encyclopedia_zh.py --check   (no write; report counts)
"""
import argparse
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / 'runtime' / 'encyclopedia_data.h'

# Order matters: longer phrases first, then stat labels.
PHRASES = [
    ('This Quest is for experienced Special Permit holders only', '此任务仅限持有特殊许可的资深猎人'),
    ('This Quest is for hunters with Special Permits', '此任务需特殊许可'),
    ('A Decoration that boosts', '可强化'),
    ('Decoration', '装饰品'),
    ('State:', '状态:'),
    ('Sub:', '副任务:'),
    ('Goal:', '目标:'),
    ('None', '无'),
    ('Normal', '普通'),
    ('Enraged', '愤怒'),
    ('Hard Lava', '熔岩硬化'),
    ('No Lava', '无熔岩'),
    ('Dark Wind', '黑风'),
    ('Red Energy', '红色能量'),
    ('Blue Energy', '蓝色能量'),
    ('Charged', '充能'),
    ('Ice Armor', '冰甲'),
    ('Broke Legs', '破坏腿部'),
    ('No Snow', '无积雪'),
    ('Snow', '积雪'),
    ('No Hide', '无皮'),
    ('Mud', '泥'),
    ('Frenzy', '狂龙病毒'),
    ('Hyper', '狞猛'),
    ('Materials', '素材'),
    ('Hunt', '狩猎'),
    ('Slay', '讨伐'),
    ('Repel', '击退'),
    ('Capture', '捕获'),
    ('Deliver', '缴纳'),
    ('Subquest', '副任务'),
    ('Time limit', '限制时间'),
    ('Reward', '报酬'),
    ('Location', '地点'),
    ('Rarity', '稀有度'),
    ('Carry', '携带数'),
    ('Buy', '购买'),
    ('Sell', '出售'),
]
ELEMENTS = {'Fire': '火', 'Water': '水', 'Thunder': '雷', 'Ice': '冰', 'Dragon': '龙'}


def translate(text: str) -> str:
    for en, zh in PHRASES:
        text = text.replace(en, zh)
    # "Fire +3", "Water 5", "Ice -2"; keep the sign/number.
    def elem(m):
        return ELEMENTS[m.group(1)] + m.group(2)
    text = re.sub(r'\b(Fire|Water|Thunder|Ice|Dragon)\b(\s*[+-]?\d)', elem, text)
    # Bare element names left in weakness lines.
    text = re.sub(r'\b(Fire|Water|Thunder|Ice|Dragon)\b', lambda m: ELEMENTS[m.group(1)], text)
    return text


def load():
    src = HEADER.read_text(encoding='utf-8')
    m = re.search(r'encyclopedia_blob\[\]\s*=\s*\{(.*?)\};', src, re.S)
    blob = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1)))
    assert blob[:8] == b'MHGUEZ1\0', blob[:8]
    raw_size, comp = struct.unpack_from('<II', blob, 8)
    raw = bytearray(zlib.decompress(blob[16:16 + comp]))
    assert len(raw) == raw_size
    return raw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    raw = load()
    ver, cats, entries, pool = struct.unpack_from('<IIII', raw, 8)
    w, h, cells, icon_off, rect_off = struct.unpack_from('<IIIII', raw, 24)
    assert ver == 2
    cat_off = 44
    ent_off = cat_off + cats * 24
    table_end = ent_off + entries * 32

    def string(off):
        end = raw.index(0, off)
        return bytes(raw[off:end]).decode('utf-8')

    # Keep the icon block as it is.
    icon_bytes = bytes(raw[icon_off:icon_off + w * h * 4])
    rect_bytes = bytes(raw[rect_off:rect_off + cells * 8])

    cats_out = []
    for i in range(cats):
        cid, first, count, en, zh, reserved = struct.unpack_from('<IIIIII', raw, cat_off + i * 24)
        cats_out.append([cid, first, count, string(en), string(zh), reserved])
    ents_out = []
    changed = 0
    for i in range(entries):
        cid, rid, en, zh, den, dzh, icon, color = struct.unpack_from('<IIIIIIII', raw, ent_off + i * 32)
        en_name, zh_name = string(en), string(zh)
        en_detail, zh_detail = string(den), string(dzh)
        new_zh = translate(zh_detail)
        if new_zh != zh_detail:
            changed += 1
        ents_out.append([cid, rid, en_name, zh_name, en_detail, new_zh, icon, color])
    if a.check:
        print(f'{cats} categories, {entries} entries, {changed} Chinese details changed')
        return

    # Rebuild: header, fixed tables, one string pool, then the icon sheet and rects.
    out = bytearray(b'MHGUEB1\0' + b'\0' * 36)
    out += b'\0' * (cats * 24 + entries * 32)
    pool = bytearray()
    pool_base = table_end

    def put(text):
        off = pool_base + len(pool)
        pool.extend(text.encode('utf-8') + b'\0')
        return off

    for i, c in enumerate(cats_out):
        c[3], c[4] = put(c[3]), put(c[4])
        struct.pack_into('<IIIIII', out, cat_off + i * 24, *c)
    for i, e in enumerate(ents_out):
        cid, rid, en_name, zh_name, en_detail, zh_detail, icon, color = e
        en_name, zh_name = put(en_name), put(zh_name)
        en_detail, zh_detail = put(en_detail), put(zh_detail)
        struct.pack_into('<IIIIIIII', out, ent_off + i * 32, cid, rid, en_name, zh_name, en_detail, zh_detail, icon, color)

    new_icon_off = len(out) + len(pool)
    out += pool + icon_bytes
    new_rect_off = len(out)
    out += rect_bytes
    # Header: magic 8, version, categories, entries, pool size, icons, offsets.
    struct.pack_into('<IIIIIIIII', out, 8, 2, cats, entries, len(pool), w, h, cells, new_icon_off, new_rect_off)
    data = zlib.compress(bytes(out), 9)
    blob = b'MHGUEZ1\0' + struct.pack('<II', len(out), len(data)) + data
    lines = ['// Embedded MHGU encyclopedia data: compressed string/record/icon table.',
             '// Compiled in directly; do not edit by hand.',
             '#pragma once',
             'static const unsigned char encyclopedia_blob[] = {']
    for i in range(0, len(blob), 16):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in blob[i:i + 16]) + ',')
    lines.append('};')
    lines.append(f'static const unsigned int encyclopedia_blob_size = {len(blob)};')
    HEADER.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'rewrote {HEADER.relative_to(ROOT)}: {changed} Chinese details, {len(blob)} bytes')


if __name__ == '__main__':
    main()
