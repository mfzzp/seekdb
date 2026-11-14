#!/usr/bin/env python3
"""Run the C++ publication fault harness and inspect each manifest after restart."""
import argparse
from pathlib import Path
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',required=True);ap.add_argument('--root',type=Path,required=True)
a=ap.parse_args();a.root.mkdir(parents=True,exist_ok=False)
binary=str(Path(a.binary).resolve())
for case in ['ok','sst_write','manifest_write','rename','sync_file','sync_dir_before','sync_dir_after','crash_before','crash_after']:
    data=(a.root/case).resolve().as_posix()
    result=subprocess.run([binary,data,case],check=False)
    assert result.returncode=={'crash_before':91,'crash_after':92}.get(case,0),(case,result.returncode)
    subprocess.run([binary,data,'inspect'],check=True)
print('PASS 9 publication/failure cases, each inspected by a restarted engine',flush=True)
