#!/usr/bin/env python3
"""Reuse the independent SQL model with geometry, column switches and GC/restart."""
from pathlib import Path
import subprocess
source=Path(__file__).with_name('exact_paths_check.py').read_text()
source=source.replace("['scan','block','auto']","['geo']")
source=source.replace("if mode!='block':","if mode not in ['block','geo']:")
source=source.replace("if mode=='block' and col", "if mode in ['block','geo'] and col")
source=source.replace('verify();s.close();s=Session',"verify();s.close();subprocess.run([str(Path(a.binary).resolve()),'--data-dir',root,'--maintenance','gc'],check=True,stdout=subprocess.DEVNULL);s=Session")
exec(compile(source,__file__,'exec'))
