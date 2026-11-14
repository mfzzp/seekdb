#!/usr/bin/env python3
"""SQL doubled quotes must survive canonical WAL replay, UPDATE, DELETE, rollback and checkpoint."""
import argparse
from pathlib import Path
import tempfile
from protocol_adapter import Session


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', required=True)
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    assert '/zzp/' in str(root)
    root.mkdir(parents=True, exist_ok=True)
    for checkpoint in [False, True]:
        data = tempfile.mkdtemp(prefix='quotes-', dir=root)
        s = Session(args.binary, data)
        s.ask('CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(64), embedding VECTOR(2));')
        s.ask("INSERT INTO docs VALUES (1,'it''s;中文','[1,2]'),(2,'delete''me','[3,4]');")
        if checkpoint:
            s.close(); s = Session(args.binary, data)
        s.ask("UPDATE docs SET title='new''title',embedding='[5,6]' WHERE title='it''s;中文';")
        s.ask("DELETE FROM docs WHERE title='delete''me';")
        s.ask('BEGIN;')
        s.ask("UPDATE docs SET title='rolled''back' WHERE id=1;")
        s.ask('ROLLBACK;')
        s.p.kill(); s.p.wait(timeout=10)
        s = Session(args.binary, data)
        assert s.ask('SELECT id,title,embedding FROM docs ORDER BY id;')['rows'] == [[1, "new'title", [5,6]]]
        assert s.ask("SELECT id FROM docs WHERE title='new''title';")['rows'] == [[1]]
        s.close()
        s = Session(args.binary, data)
        assert s.ask('SELECT id,title,embedding FROM docs ORDER BY id;')['rows'] == [[1, "new'title", [5,6]]]
        s.close()
        print('sql_literal_recovery checkpoint=%s PASS' % checkpoint, flush=True)


if __name__ == '__main__':
    main()
