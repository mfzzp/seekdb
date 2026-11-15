#!/usr/bin/env python3
"""Lose unsynced vector bytes despite an acknowledged WAL commit, then recover.

This is a controlled loss model, not a physical power-cut experiment. All copied
directories are private fixtures; a kill by itself leaves Linux page cache alive.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
binary=str(Path(a.binary).resolve());results=[]
def call(data,mode,code=0):
    p=subprocess.run([binary,str(data),mode],capture_output=True,text=True,timeout=120)
    assert p.returncode==code,(data.name,mode,p.returncode,p.stdout,p.stderr)
    return json.loads(p.stdout.splitlines()[-1]) if p.stdout else None
def record(name,**more):
    results.append(dict(case=name,passed=True,**more));print(json.dumps(results[-1]),flush=True)
    (a.root/'result.json').write_text(json.dumps(dict(passed=True,cases=results),indent=2)+'\n')
def crc32c(data):
    c=0xffffffff
    for b in data:
        c^=b
        for _ in range(8):c=(c>>1)^(0x82f63b78 if c&1 else 0)
    return c^0xffffffff

for packed in ['0','1']:
    os.environ['LITE_WAL_BATCH']=packed
    os.environ['LITE_WAL_CACHE']='0'
    base=a.root/('seed-'+packed);meta=call(base,'seed')
    assert meta['end']>meta['durable_end'] and meta['wal_syncs']==1 and meta['vector_syncs']==0,meta
    record('one-sync-'+packed,**meta)
    for damage in ['truncate','zero_same_length','torn_same_length','extra_tail']:
        dst=a.root/(damage+'-'+packed);shutil.copytree(base,dst)
        file=dst/meta['vector']
        with file.open('r+b') as f:
            if damage=='truncate':f.truncate(meta['durable_end'])
            elif damage=='extra_tail':f.seek(0,2);f.write(b'UNCOMMITTED'*11)
            else:
                f.seek(meta['durable_end']);n=meta['end']-meta['durable_end'];raw=f.read(n)
                bad=bytes(n) if damage=='zero_same_length' else bytes(0 if i%3 else b for i,b in enumerate(raw))
                f.seek(meta['durable_end']);f.write(bad)
        for _ in range(2):call(dst,'inspect_new')
        assert file.stat().st_size==meta['end']
        record(damage+'-'+packed)
    gcbase=a.root/('generation-'+packed);gcmeta=call(gcbase,'seed_gc')
    assert gcmeta['vector'].endswith('.vectors')
    with (gcbase/gcmeta['vector']).open('r+b') as f:f.truncate(gcmeta['durable_end'])
    for _ in range(2):call(gcbase,'inspect_new')
    record('gc-generation-replay-'+packed)
    chunks=a.root/('chunks-'+packed);chunkmeta=call(chunks,'chunk_seed')
    assert chunkmeta['end']-chunkmeta['durable_end']>65536*3
    for damage in ['truncate','zero_same_length']:
        dst=a.root/('chunk-'+damage+'-'+packed);shutil.copytree(chunks,dst)
        with (dst/chunkmeta['vector']).open('r+b') as f:
            if damage=='truncate':f.truncate(chunkmeta['durable_end'])
            else:f.seek(chunkmeta['durable_end']);f.write(bytes(chunkmeta['end']-chunkmeta['durable_end']))
        for _ in range(2):call(dst,'redo_crash_later',94)
        for _ in range(2):call(dst,'chunk_inspect')
        assert (dst/chunkmeta['vector']).stat().st_size==chunkmeta['end']
        record('multi-frame-replay-'+damage+'-'+packed)
    for mode in ['redo_short','redo_eintr','redo_eio','redo_enospc','redo_crash']:
        dst=a.root/(mode+'-'+packed);shutil.copytree(base,dst)
        with (dst/meta['vector']).open('r+b') as f:f.truncate(meta['durable_end'])
        for _ in range(2 if mode=='redo_crash' else 1):call(dst,mode,94 if mode=='redo_crash' else 1 if mode in ['redo_eio','redo_enospc'] else 0)
        for _ in range(2):call(dst,'inspect_new')
        assert (dst/meta['vector']).stat().st_size==meta['end']
        record(mode+'-'+packed)
    for mode in ['vector_short','wal_short','vector_eintr','wal_eintr','sync_eintr','vector_eio','wal_eio','vector_enospc','wal_enospc','wal_sync_eio','wal_sync_enospc','after_wal','auto_sync_eio','auto_sync_enospc','auto_crash','checkpoint_vector_sync_eio','checkpoint_vector_sync_enospc','flush_vector_sync_eio','uncommitted','scalar']:
        dst=a.root/(mode+'-'+packed)
        call(dst,mode,92 if mode=='after_wal' else 93 if mode=='auto_crash' else 0)
        old=mode in ['vector_eio','wal_eio','vector_enospc','wal_enospc','scalar']
        call(dst,'inspect_old' if old else 'inspect_new')
        record(mode+'-'+packed)
    # Complete checksummed frame corruption must fail closed. A torn final
    # COMMIT must not leak its otherwise complete vector and key records.
    original=(base/meta['wal']).read_bytes();frames=[];p=0
    while p<len(original):
        n=struct.unpack_from('<I',original,p)[0];frames.append((p,n,original[p+8]));p+=4+n
    assert any(k==5 for _,_,k in frames) and frames[-1][2]==6
    for damage in ['vector_crc','vector_offset','commit_truncated']:
        dst=a.root/(damage+'-'+packed);shutil.copytree(base,dst);raw=bytearray(original)
        start,n,_=next(x for x in frames if x[2]==5)
        if damage=='vector_crc':raw[start+17]^=0x80
        elif damage=='vector_offset':
            off=struct.unpack_from('<Q',raw,start+9)[0];struct.pack_into('<Q',raw,start+9,off+4)
            struct.pack_into('<I',raw,start+4,crc32c(raw[start+8:start+4+n]))
        else:raw=raw[:-3]
        (dst/meta['wal']).write_bytes(raw)
        call(dst,'inspect_old' if damage=='commit_truncated' else 'inspect_new',0 if damage=='commit_truncated' else 1)
        record(damage+'-'+packed)
print(json.dumps(dict(passed=True,count=len(results))),flush=True)
