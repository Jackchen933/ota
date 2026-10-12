#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 UI/generated/screens 下哪些 label 没有显式设置字体（依赖 LV_FONT_DEFAULT）。"""
import re
import os

D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                 'UI', 'generated', 'screens')

for fn in sorted(os.listdir(D)):
    src = open(os.path.join(D, fn), encoding='utf-8', errors='replace').read()
    blocks = re.split(r'// Create ', src)
    for b in blocks[1:]:
        m = re.match(r'[^:]+: (\S+)', b)
        name = m.group(1) if m else '?'
        if 'label' not in name:
            continue
        if 'lv_label_create' not in b:
            continue
        has_font = 'set_style_text_font' in b
        t = re.search(r'lv_label_set_text\([^,]+,\s*"(.*?)"\)', b, re.S)
        text = t.group(1) if t else None
        if not has_font:
            print('%-18s %-30s NO-FONT text=%r' % (fn, name, text))
