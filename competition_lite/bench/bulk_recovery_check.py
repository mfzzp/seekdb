#!/usr/bin/env python3
"""Check wide INSERT WAL frames after SIGKILL, before any checkpoint."""
import argparse
import json
from pathlib import Path
import tempfile
from protocol_adapter import Session


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', required=True)
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    if '/zzp/' not in root.as_posix():
        raise ValueError('root must be inside zzp')
    root.mkdir(parents=True, exist_ok=True)
    for batch, checkpoint in [(256, False), (1024, False), (256, True), (1024, True)]:
        data = tempfile.mkdtemp(prefix='bulk-%d-' % batch, dir=root)
        session = Session(args.binary, data)
        session.ask('CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), '
                    'tenant_id BIGINT, price_cents BIGINT, created_day BIGINT, '
                    'active BIGINT, title VARCHAR(32), embedding VECTOR(4));')
        if checkpoint:
            session.ask("INSERT INTO docs VALUES (0,'c00',0,0,0,0,'base','[9,8,7,6]'),"
                        "(-1,'c00',0,0,0,0,'dead tail','[5,6,7,8]');")
            session.ask('DELETE FROM docs WHERE id=-1;')
            session.close()
            session = Session(args.binary, data)
        values = ["(%d,'c03',7,1200,100,1,'row%d','[1,2,3,4]')" % (i, i)
                  for i in range(1, batch + 1)]
        assert session.ask('INSERT INTO docs VALUES ' + ','.join(values))['affected_rows'] == batch
        session.p.kill()
        session.p.wait(timeout=10)
        assert (Path(data) / 'STATE').exists() == checkpoint
        session = Session(args.binary, data)
        found = session.ask('SELECT id FROM docs ORDER BY id LIMIT %d;' % (batch + 2))['rows']
        assert found == [[i] for i in range(0 if checkpoint else 1, batch + 1)], found
        distance = session.ask("SELECT id,L2_DISTANCE(embedding,'[1,2,3,4]') AS d "
                               "FROM docs WHERE id=%d ORDER BY d LIMIT 1;" % batch)['rows']
        assert distance == [[batch, 0]], distance
        if checkpoint:
            old = session.ask("SELECT id,L2_DISTANCE(embedding,'[9,8,7,6]') AS d "
                              "FROM docs WHERE id=0 ORDER BY d LIMIT 1;")['rows']
            assert old == [[0, 0]], old
        session.close()
        print(json.dumps({'batch': batch, 'columns': 8, 'checkpoint': checkpoint,
                          'recovered_rows': len(found), 'pass': True}), flush=True)


if __name__ == '__main__':
    main()
