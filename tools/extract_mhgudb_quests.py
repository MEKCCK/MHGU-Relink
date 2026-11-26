#!/usr/bin/env python3
"""Extract the quest ID -> localized name/goal/rank metadata used by the hub web room list.

Chinese comes from the APK's `name_zh`/`goal_zh` columns. The input is the MHGU Database APK (com.ghstudios.android.mhgendatabase), whose
``assets/databases/mhgu.db.zip`` holds the quest table. Only this small metadata
subset is written, never the full app/database.

    python3 tools/extract_mhgudb_quests.py MHGUDatabase.apk
    python3 tools/extract_mhgudb_quests.py --out crates/mhgu-hub/data/quests.json MHGUDatabase.apk
"""
import argparse
import io
import json
import sqlite3
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'crates' / 'mhgu-hub' / 'data' / 'quests.json'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('apk')
    ap.add_argument('--out', default=str(OUT))
    a = ap.parse_args()

    with zipfile.ZipFile(a.apk) as apk:
        dbzip = apk.read('assets/databases/mhgu.db.zip')
    with zipfile.ZipFile(io.BytesIO(dbzip)) as z:
        db = z.read('mhgu.db')
    with tempfile.NamedTemporaryFile(suffix='.db') as f:
        f.write(db)
        f.flush()
        c = sqlite3.connect(f.name)
        rows = c.execute(
            'SELECT _id, name, name_es, name_zh, name_ja, goal, goal_es, goal_zh, goal_ja, hub, rank, stars '
            'FROM quests ORDER BY _id'
        ).fetchall()
        c.close()

    quests = {}
    for qid, name, name_es, name_zh, name_ja, goal, goal_es, goal_zh, goal_ja, hub, rank, stars in rows:
        quests[str(qid)] = {
            'name': name,
            'name_es': name_es,
            'name_zh': name_zh,
            'name_ja': name_ja,
            'goal': goal,
            'goal_es': goal_es,
            'goal_zh': goal_zh,
            'goal_ja': goal_ja,
            'hub': hub,
            'rank': rank,
            'stars': stars,
        }
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({'source': 'MHGUDatabase 2.4.5', 'quests': quests},
                              ensure_ascii=False, separators=(',', ':'), sort_keys=True) + '\n',
                  encoding='utf-8')
    print(f'{out}: {len(quests)} quests, {out.stat().st_size} bytes')


if __name__ == '__main__':
    main()
