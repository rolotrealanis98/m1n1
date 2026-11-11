#!/usr/bin/env python3
"""Additional stage inspection at the new private command-construction function.
The inherited stage_inspection.py remains unchanged and its failure is reported.
"""
from pathlib import Path
p=Path(__file__).with_name('stage_inspection.py')
s=p.read_text().replace('define zeroext i1 @ans_ro_xfer(', 'define internal zeroext i1 @xfer_owned(')
exec(compile(s,str(p),'exec'),{'__file__':str(p),'__name__':'__main__'})
