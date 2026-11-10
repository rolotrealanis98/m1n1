#!/usr/bin/env python3
"""Copy the r2 resident model sources into the S0/S1 suite copies and re-pin their manifests."""
import hashlib,json,shutil
from pathlib import Path
R=Path(__file__).resolve().parents[1]; M=R/'source/src/hv/win/s1'; T=R/'tests/storage'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for n in ['hv_nvme_queue.c','hv_nvme_queue.h','ram_disk.c','ram_disk.h']: shutil.copyfile(M/n,T/'s0/src'/n)
for n in ['hv_nvme.c','hv_pci.c','model.h']: shutil.copyfile(M/n,T/'s1/src'/n)
for s in ['s0','s1']:
    mf=json.loads((T/s/'MANIFEST.json').read_text())
    for k in list(mf['files']): mf['files'][k]=sha(T/s/k) if (T/s/k).exists() else mf['files'][k]
    (T/s/'MANIFEST.json').write_text(json.dumps(mf,indent=2)+'\n')
mf=json.loads((T/'s1/MANIFEST.json').read_text())
for k in list(mf['s0_dependencies']): mf['s0_dependencies'][k]=sha(T/'s0'/k)
(T/'s1/MANIFEST.json').write_text(json.dumps(mf,indent=2)+'\n')
print('synced')
