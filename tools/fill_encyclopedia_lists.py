#!/usr/bin/env python3
"""Fill the encyclopedia's truncated lists from MHGU Database 2.4.5's mhgu.db.

The embedded data caps long lists (monster quests, item quest rewards, gathering sites,
combinations) with "... and N more" / "另有 N 个" / "(more sites not shown)". This tool
rebuilds those sections in full from the database and writes runtime/encyclopedia_data.h
back, keeping the same binary layout (header, category/entry tables, string pool, icons).

    python3 tools/fill_encyclopedia_lists.py [MHGUDatabase.apk]
"""
import argparse
import io
import re
import sqlite3
import struct
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / 'runtime' / 'encyclopedia_data.h'
DEFAULT_APK = Path.home() / 'Downloads' / 'MHGUDatabase.apk.1'

RANK_EN = {'LR': 'LR', 'HR': 'HR', 'G': 'G'}
RANK_LONG_EN = {'LR': 'Low Rank', 'HR': 'High Rank', 'G': 'G Rank'}
RANK_ZH = {'LR': '下位', 'HR': '上位', 'G': 'G位'}
SITE_EN = {
    '采集': 'Gather', '采矿': 'Mine', '捕虫': 'Bug',
    '钓鱼': 'Fish', '钓鱼 (黄金团子)': 'Fish (Goldenfish bait)',
    '钓鱼 (狂热蝇钓饵)': 'Fish (frenzy bait)', '钓鱼 (刺身鱼团子)': 'Fish (Sushifish bait)',
    '钓鱼 (炸弹鱼团子)': 'Fish (Bomb Arowana bait)',
}


def load_blob():
    src = HEADER.read_text(encoding='utf-8')
    m = re.search(r'encyclopedia_blob\[\]\s*=\s*\{(.*?)\};', src, re.S)
    blob = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1)))
    raw = bytearray(zlib.decompress(blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]))
    return raw


def db_from_apk(path):
    with zipfile.ZipFile(path) as apk:
        zipped = apk.read('assets/databases/mhgu.db.zip')
    with zipfile.ZipFile(io.BytesIO(zipped)) as z:
        db_bytes = z.read('mhgu.db')
    f = tempfile.NamedTemporaryFile(suffix='.db', delete=False)
    f.write(db_bytes)
    f.close()
    return sqlite3.connect(f.name), Path(f.name)


def replace_section(text, headers, lines):
    """Replace a section (header line through the next blank line) with lines."""
    rows = text.split('\n')
    for i, row in enumerate(rows):
        if row.strip() not in headers:
            continue
        j = i + 1
        while j < len(rows) and rows[j].strip() != '':
            j += 1
        rows[i + 1:j] = lines
        return '\n'.join(rows)
    return text


def has_marker(section):
    return any(m in section for m in ('另有', 'more', '仅显示', 'more sites', '%%%'))


def section_text(text, header):
    rows = text.split('\n')
    for i, row in enumerate(rows):
        if row.strip() == header:
            j = i + 1
            while j < len(rows) and rows[j].strip() != '':
                j += 1
            return '\n'.join(rows[i + 1:j])
    return ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('apk', nargs='?', default=str(DEFAULT_APK))
    a = ap.parse_args()
    db, tmp = db_from_apk(a.apk)
    q = lambda sql, *args: db.execute(sql, args).fetchall()

    item_name = {i: (en, zh) for i, en, zh, in q('select _id,name,name_zh from items')}
    quest_name = {i: (en, zh) for i, en, zh in q('select _id,name,name_zh from quests')}
    monster_name = {i: (en, zh) for i, en, zh in q('select _id,name,name_zh from monsters')}
    loc_name = {i: (en, zh) for i, en, zh in q('select _id,name,name_zh from locations')}

    monster_quests = {}
    for mid, qid, rank, unstable in q(
            'select monster_id, quest_id, rank, unstable from monster_to_quest '
            'join quests on quests._id = monster_to_quest.quest_id order by monster_to_quest._id'):
        monster_quests.setdefault(mid, []).append((qid, rank))
    rewards = {}
    for iid, qid, slot, stack, pct in q(
            'select item_id, quest_id, reward_slot, stack_size, percentage from quest_rewards order by _id'):
        rewards.setdefault(iid, []).append((qid, slot, stack, pct))
    gather = {}
    seen = {}
    for iid, lid, area, site, rank in q(
            'select item_id, location_id, area, site, rank from gathering order by _id'):
        key = (iid, lid, area, site, rank)
        if key in seen:
            continue
        seen[key] = 1
        gather.setdefault(iid, []).append((lid, area, site, rank))
    combine = {}
    for cid, made, i1, i2, lo, hi, pct in q(
            'select created_item_id, created_item_id, item_1_id, item_2_id, amount_made_min, amount_made_max, percentage from combining order by _id'):
        combine.setdefault(i1, []).append(('use', made, i2, lo, hi, pct))
        combine.setdefault(i2, []).append(('use', made, i1, lo, hi, pct))
        combine.setdefault(made, []).append(('made', made, i1, i2, lo, hi, pct))
    hunt = {}
    for iid, mid, cond, pct in q(
            'select item_id, monster_id, condition, percentage from hunting_rewards order by _id'):
        hunt.setdefault(iid, []).append((mid, cond, pct))

    raw = load_blob()
    ver, cats, entries, pool = struct.unpack_from('<IIII', raw, 8)
    w, h, cells, icon_off, rect_off = struct.unpack_from('<IIIII', raw, 24)
    cat_off = 44
    ent_off = cat_off + cats * 24
    table_end = ent_off + entries * 32

    def string(off):
        return bytes(raw[off:raw.index(0, off)]).decode('utf-8')

    icons = bytes(raw[icon_off:icon_off + w * h * 4])
    rects = bytes(raw[rect_off:rect_off + cells * 8])

    # English condition wording used by the original generator, recovered by aligning the
    # still-truncated English/Chinese sections with the database rows.
    condition_en = {}
    for i in range(entries):
        c, rid, en, zh, den, dzh, icon, color = struct.unpack_from('<IIIIIIII', raw, ent_off + i * 32)
        if c != 1 or rid not in hunt:
            continue
        for line, (mid, cond, pct) in zip(section_text(string(den), 'Monster rewards').split('\n'), hunt[rid]):
            m2 = re.match(r'\s*(.*?) / (.*?) (\d+)%+\s*$', line)
            if m2:
                condition_en.setdefault(cond, m2.group(2))
    cats_out, ents_out, changed = [], [], 0
    for i in range(cats):
        c = list(struct.unpack_from('<IIIIII', raw, cat_off + i * 24))
        c[3], c[4] = string(c[3]), string(c[4])
        cats_out.append(c)
    for i in range(entries):
        c, rid, en, zh, den, dzh, icon, color = struct.unpack_from('<IIIIIIII', raw, ent_off + i * 32)
        en_detail, zh_detail = string(den), string(dzh)
        if c == 0:  # monsters: the quest list
            for lang in ('en', 'zh'):
                text = en_detail if lang == 'en' else zh_detail
                header = 'Quests' if lang == 'en' else '任务'
                sec = section_text(text, header)
                if sec and has_marker(sec):
                    rows = []
                    for qid, rank in monster_quests.get(rid, []):
                        name = quest_name.get(qid, ('', ''))[0 if lang == 'en' else 1]
                        rows.append(f'  {name} ({rank}) ' + ('[unstable]' if lang == 'en' else '[不稳定]'))
                    text = replace_section(text, (header,), rows)
                    if lang == 'en': en_detail = text
                    else: zh_detail = text
                    changed += 1
        elif c == 1:  # items
            for lang in ('en', 'zh'):
                text = en_detail if lang == 'en' else zh_detail
                hdr_rewards = 'Quest rewards' if lang == 'en' else '任务报酬'
                sec = section_text(text, hdr_rewards)
                if sec and has_marker(sec):
                    rows = []
                    for qid, slot, stack, pct in rewards.get(rid, []):
                        name = quest_name.get(qid, ('', ''))[0 if lang == 'en' else 1]
                        rows.append(f'  {name} [{slot}] x{stack} {pct}%')
                    text = replace_section(text, (hdr_rewards,), rows)
                    changed += 1
                hdr_gather = 'Gathering' if lang == 'en' else '采集'
                sec = section_text(text, hdr_gather)
                if sec and has_marker(sec):
                    rows = []
                    for lid, area, site, rank in gather.get(rid, []):
                        loc = loc_name.get(lid, ('', ''))[0 if lang == 'en' else 1]
                        rows.append(f'  {loc} / {area} / {SITE_EN.get(site, site) if lang == "en" else site} / '
                                    f'{RANK_LONG_EN.get(rank, rank) if lang == "en" else RANK_ZH.get(rank, rank)}')
                    text = replace_section(text, (hdr_gather,), rows)
                    changed += 1
                hdr_hunt = 'Monster rewards' if lang == 'en' else '怪物报酬'
                sec = section_text(text, hdr_hunt)
                if sec and has_marker(sec):
                    rows = []
                    for mid, cond, pct in hunt.get(rid, []):
                        name = monster_name.get(mid, ('', ''))[0 if lang == 'en' else 1]
                        if lang == 'en':
                            cond = condition_en.get(cond, cond)
                        rows.append(f'  {name} / {cond} {pct}%')
                    text = replace_section(text, (hdr_hunt,), rows)
                    changed += 1
                hdr_comb = 'Combinations' if lang == 'en' else '调和'
                sec = section_text(text, hdr_comb)
                if sec and has_marker(sec):
                    made_lines, use_lines = [], []
                    for row in combine.get(rid, []):
                        if row[0] == 'made':
                            _, made, i1, i2, lo, hi, pct = row
                            a1 = item_name.get(i1, ('', ''))[0 if lang == 'en' else 1]
                            a2 = item_name.get(i2, ('', ''))[0 if lang == 'en' else 1]
                            if lang == 'en':
                                use_lines.append(f'  Made from {a1} + {a2} ({lo}-{hi}, {pct}%)')
                            else:
                                use_lines.append(f'  调和自 {a1} + {a2} ({lo}-{hi}, {pct}%)')
                        else:
                            _, made, other, lo, hi, pct = row
                            a1 = item_name.get(made, ('', ''))[0 if lang == 'en' else 1]
                            if lang == 'en':
                                made_lines.append(f'  Used in {a1} ({pct}%)')
                            else:
                                made_lines.append(f'  用于调和 {a1} ({pct}%)')
                    rows = list(dict.fromkeys(use_lines + made_lines))
                    text = replace_section(text, (hdr_comb,), rows)
                    changed += 1
                if lang == 'en': en_detail = text
                else: zh_detail = text
        ents_out.append([c, rid, string(en), string(zh), en_detail, zh_detail, icon, color])

    out = bytearray(b'MHGUEB1\0' + b'\0' * 36)
    out += b'\0' * (cats * 24 + entries * 32)
    pool = bytearray()
    base = table_end

    def put(s):
        off = base + len(pool)
        pool.extend(s.encode('utf-8') + b'\0')
        return off

    for i, c in enumerate(cats_out):
        c[3], c[4] = put(c[3]), put(c[4])
        struct.pack_into('<IIIIII', out, cat_off + i * 24, *c)
    for i, e in enumerate(ents_out):
        c, rid, en, zh, den, dzh, icon, color = e
        en, zh = put(en), put(zh)
        den, dzh = put(den), put(dzh)
        struct.pack_into('<IIIIIIII', out, ent_off + i * 32, c, rid, en, zh, den, dzh, icon, color)
    new_icon = len(out) + len(pool)
    out += pool + icons
    new_rect = len(out)
    out += rects
    struct.pack_into('<IIIIIIIII', out, 8, 2, cats, entries, len(pool), w, h, cells, new_icon, new_rect)
    blob = b'MHGUEZ1\0' + struct.pack('<II', len(out), len(zlib.compress(bytes(out), 9))) + zlib.compress(bytes(out), 9)
    lines = ['// Embedded MHGU encyclopedia data: compressed string/record/icon table.',
             '// Compiled in directly; do not edit by hand.', '#pragma once',
             'static const unsigned char encyclopedia_blob[] = {']
    for i in range(0, len(blob), 16):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in blob[i:i + 16]) + ',')
    lines += ['};', f'static const unsigned int encyclopedia_blob_size = {len(blob)};']
    HEADER.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'{changed} sections filled, {len(blob)} bytes')
    db.close()
    tmp.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
