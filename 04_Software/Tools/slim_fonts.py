#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
字体子集化 —— 从 GUI Guider 导出的完整字体 .c 里，只保留界面真正用到的字形。

为什么需要
----------
GUI Guider 给每个字体都导出 63~66 个字形（ASCII 全套数字+大小写字母，CJK 字体
再额外带几个汉字），但 022 这套界面总共只用到 30 个不同字符。7 个字体合计
209,184 字节位图，其中 189 KB 是永远画不到的。

做法
----
不重新生成字体，直接在已生成的 .c 上做"外科手术"，保证保留下来的字形与
原版逐字节相同（画质零损失）：

  1. 解析原文件的 glyph_bitmap / glyph_dsc / cmaps / kerning
  2. 按 字符 -> 原 glyph id 找到要保留的字形
  3. 按码点升序重排成新的 glyph id 1..N
  4. 位图按原样切片（每个字形占 box_h * stride 字节，stride = 16；
     该不变式已对 7 个字体逐个校验过，全部成立）
  5. 重建 cmaps 为单个 SPARSE_TINY；重建 kerning 的 gid->class 映射数组
  6. font_dsc 的 bpp / stride / line_height 等字段保持原值

一个关键的坑
------------
被丢弃的字形必须是"整个 glyph_dsc 条目不存在"，不能是"box_w/box_h 置 0"。

因为 src/font/lv_font.c 的 lv_font_get_glyph_dsc() 是这么写的：

    while(f) {
        bool found = f->get_glyph_dsc(f, dsc_out, letter, ...);
        if(found) {
            if(!dsc_out->is_placeholder) { dsc_out->resolved_font = f; return true; }
        }
        f = f->fallback;
    }

只要描述符拿得到且 is_placeholder == false 就直接采用本字体，**不会**因为
box_w == 0 而回退。而 lv_font_get_bitmap_fmt_txt() 遇到 box_w*box_h == 0
只是 return NULL —— 结果是画出一个空白间隙，既不报错也没有字形。

所以必须让该字符在 cmap 里查不到（get_glyph_dsc_id 返回 0），描述符函数才会
返回 false，循环才会走到 f->fallback。每个字体的 fallback 都是
lv_font_montserrat_14_aligned，所以以后在 GUI Guider 里改了文字、char 一旦
不在子集内，会以 montserrat_14 的字形显示出来（字号不对但看得见），而不是
静默留白。这也是为什么下面要重建 cmaps 而不是简单地砍位图。

用法
----
    python Tools/slim_fonts.py

输入  generated/assets/fonts/*.c   （GUI Guider 原样导出，不要手改）
输出  UI/assets/fonts/*.c          （本脚本生成，可重复运行）
"""

import io
import os
import re
import sys

# ---------------------------------------------------------------- 配置

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, 'generated', 'assets', 'fonts')
DST_DIR = os.path.join(ROOT, 'UI', 'assets', 'fonts')

# 每个字体需要渲染的**全部**字符串。
# 来源：逐条 grep generated/screens/*.c 里的 lv_label_set_text / lv_arclabel_set_text
# 注意 gg_screen_2.c 用的是 lv_arclabel_set_text（"深圳"），只 grep lv_label_set_text 会漏。
FONT_TEXT = {
    'lv_font_interttf_30':              ['AM', '22', '10', '09/22'],
    'lv_font_interttf_40':              ['LIXIN'],
    'lv_font_montserratMedium_10':      ['304'],
    'lv_font_digitaldreamfatnarrow_30': ['13:24'],
    'lv_font_alimama_30':               ['19 周日'],
    'lv_font_alimama_14':               ['深圳'],
    'lv_font_montserratMedium_20':      ['10.30 am'],
}

# 额外保留的字符：数字全留。
# 理由——界面上的数字（时钟 13:24、日期 09/22、温湿度 304）是最可能改成运行时
# 动态显示的，提前留出来省得以后每次改数值都要重跑本脚本。
#
# 例外：下面这些字体的界面文本是固定的（纯字母/纯汉字），数字永远用不到，
# 保留只会白占 Flash。加进来就覆盖默认的全部保留。
EXTRA_CHARS = '0123456789'
EXTRA_CHARS_BY_FONT = {
    'lv_font_interttf_40': '',   # 只显示 "LIXIN"
    'lv_font_alimama_14': '',    # 只显示 "深圳"
}

# GUI Guider 给的原版字体，.fallback 指向 lv_font_montserrat_14_aligned（30,032 字节）。
# 但那个字体**整个 016 工程里没有任何东西引用**，也就是说它根本不在当前 ROM 基线里，
# 一旦被这 7 个子集字体的 fallback 拖进来，就是白加 30 KB。
#
# 换成 lv_font_montserrat_14 就好了：它是 lv_conf.h 里的 LV_FONT_DEFAULT，
# 被 src/display/lv_display.c 的主题初始化引用着，**本来就在基线里**。
# 两者字形完全相同（aligned 版只是把每行补齐到 16 字节，画出来一模一样），
# 换过去等于净省 30,032 字节。
#
# 022 移植时进一步发现： montserrat_14 本身也只是被上面的 fallback 和
# LV_FONT_DEFAULT 引用着，022 的所有 label 都显式指定了字体。
# 所以 LV_FONT_DEFAULT 改成 montserratMedium_10、这里 fallback 直接置 NULL，
# 链接器就能把整个 montserrat_14（约 11 KB）从 Flash 里丢掉。
# 代价：界面文本一旦用了子集外的字符，会显示成空白（而不是别的字形）。
# LVGL 对 .fallback = NULL 处理正常（lv_font_get_glyph_dsc 的 while(f) 循环）。
FALLBACK_FONT = None

# ---------------------------------------------------------------- 解析

GLYPH_DSC_RE = re.compile(
    r'\.bitmap_index\s*=\s*(\d+),\s*'
    r'\.adv_w\s*=\s*(-?\d+),\s*'
    r'\.box_w\s*=\s*(\d+),\s*'
    r'\.box_h\s*=\s*(\d+),\s*'
    r'\.ofs_x\s*=\s*(-?\d+),\s*'
    r'\.ofs_y\s*=\s*(-?\d+)')


def _extract_array_body(text, name):
    """取出 `... name[] = { ... };` 的花括号内容。"""
    m = re.search(re.escape(name) + r'\[\]\s*=\s*\{(.*?)\n\};', text, re.S)
    if m is None:
        raise ValueError('找不到数组 %s' % name)
    return m.group(1)


def _parse_cmap_entries(body):
    """解析 cmaps[] 的每个 {...} 块（属性顺序不固定，逐个用正则取）。"""
    cmaps = []
    for blk in re.finditer(r'\{([^{}]*)\}', body):
        t = blk.group(1)

        def field(key, default=None):
            mm = re.search(r'\.' + key + r'\s*=\s*([A-Za-z0-9_x]+)', t)
            return mm.group(1) if mm else default

        cmaps.append({
            'range_start': int(field('range_start', '0'), 0),
            'range_length': int(field('range_length', '0'), 0),
            'glyph_id_start': int(field('glyph_id_start', '0'), 0),
            'unicode_list': field('unicode_list'),
            'list_length': int(field('list_length', '0'), 0),
            'type': field('type', ''),
        })
    return cmaps


def parse_font(path):
    """把一个字体 .c 拆成结构化数据。"""
    text = open(path, encoding='utf-8', errors='replace').read()

    f = {}

    # ---- 文件头（版权注释 + #include 段），原样保留到 glyph_bitmap 之前
    cut = text.index('static LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {')
    f['header'] = text[:cut]

    # ---- 位图原始字节
    body = _extract_array_body(text, 'glyph_bitmap')
    f['bitmap'] = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))

    # ---- glyph_dsc（含 id = 0 的 reserved 条目）
    body = _extract_array_body(text, 'glyph_dsc')
    f['glyph_dsc'] = [tuple(int(x) for x in m.groups())
                      for m in GLYPH_DSC_RE.finditer(body)]

    # ---- cmaps 及其引用的 sparse unicode_list
    f['cmaps'] = _parse_cmap_entries(_extract_array_body(text, 'cmaps'))
    f['unicode_lists'] = {}
    for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
        f['unicode_lists'][m.group(1)] = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', m.group(2))]

    # ---- font_dsc 里的标量字段
    for key in ('bpp', 'stride', 'kern_scale', 'kern_classes'):
        m = re.search(r'\.' + key + r'\s*=\s*(-?\d+)', text)
        f[key] = int(m.group(1)) if m else 0

    # ---- kerning（只有 alimama 两个字体会带）
    km = re.search(r'\.kern_dsc\s*=\s*&(\w+)', text)
    f['has_kern'] = km is not None
    if f['has_kern']:
        gm = re.search(r'\.left_class_cnt\s*=\s*(\d+)', text)
        f['left_class_cnt'] = int(gm.group(1))
        f['right_class_cnt'] = int(re.search(r'\.right_class_cnt\s*=\s*(\d+)', text).group(1))
        for arr in ('kern_left_class_mapping', 'kern_right_class_mapping', 'kern_class_values'):
            f[arr] = [int(x) for x in re.findall(
                r'-?\d+', _extract_array_body(text, arr))]

    # ---- PUBLIC FONT 段（含 lv_font_t 定义）原样保留
    pm = re.search(r'/\*-+\n \*  PUBLIC FONT\n \*-+', text)
    f['public_font'] = text[pm.start():]

    # ---- 文件尾（#endif 收尾）
    f['tail'] = text[text.rindex('#endif /*#if'):]

    return f


def cp_to_gid(font, cp, font_name):
    """按 LVGL 的 get_glyph_dsc_id 规则，把码点翻成原始 glyph id；找不到返回 0。"""
    for cm in font['cmaps']:
        rcp = cp - cm['range_start']
        if rcp < 0 or rcp >= cm['range_length']:
            continue
        if cm['type'] == 'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY':
            return cm['glyph_id_start'] + rcp
        if cm['type'] == 'LV_FONT_FMT_TXT_CMAP_SPARSE_TINY':
            lst = font['unicode_lists'].get(cm['unicode_list'], [])
            for k, off in enumerate(lst):
                if off == rcp:
                    return cm['glyph_id_start'] + k
    return 0


def glyph_bytes(font, gid):
    """第 gid 个字形的位图字节区间 [起, 止)。

    stride 是字体级常量，每个字形占 box_h * round_up(box_w, stride) 字节。
    该不变式已逐字体校验过（校验脚本见 Tools/verify_slim.py）。
    """
    dsc = font['glyph_dsc']
    start = dsc[gid][0]
    if gid + 1 < len(dsc):
        end = dsc[gid + 1][0]
    else:
        end = len(font['bitmap'])
    return start, end


# ---------------------------------------------------------------- 生成

def _c_char_comment(cp):
    """把码点写成 C 注释里的可视字符；控制字符/引号/反斜杠做转义。"""
    ch = chr(cp)
    if ch == '"':
        return '\\"'
    if ch == '\\':
        return '\\\\'
    if cp < 0x20 or cp == 0x7F:
        return '\\x%02x' % cp
    return ch


def emit(font_name, font):
    """按 LVGL 自家生成器的排版风格输出子集后的 .c。"""
    dsc = font['glyph_dsc']
    bpp = font['bpp']
    stride = font['stride']

    # ---- 1. 决定保留哪些字符，并翻成原始 gid
    wanted = set(EXTRA_CHARS_BY_FONT.get(font_name, EXTRA_CHARS))
    missing = []
    for s in FONT_TEXT[font_name]:
        for ch in s:
            wanted.add(ch)
    # 只保留原字体 cmap 里确实有的字符
    #
    # 注意：box_w/box_h 为 0 的字形是**合法的空白字形**（空格就是），不能当成
    # "缺字形"丢掉——它占 0 字节位图，留在 cmap 里才能保持和原版一致的行为。
    kept = []            # [(cp, old_gid)]
    for cp in sorted(ord(c) for c in wanted):
        gid = cp_to_gid(font, cp, font_name)
        if gid == 0:
            missing.append(cp)
            continue
        kept.append((cp, gid))

    # 界面真正要显示、却压根没有字形的字符 = 真问题，必须报出来
    critical = sorted({ord(c) for s in FONT_TEXT[font_name] for c in s} -
                      {cp for cp, _ in kept})
    if critical:
        raise SystemExit('%s: 界面用到但字体里找不到字形: %s'
                         % (font_name, ' '.join('U+%04X' % c for c in critical)))

    # ---- 2. 重排成新 gid 1..N
    out_bitmap = bytearray()
    new_dsc = [(0, 0, 0, 0, 0, 0)]     # id 0 reserved
    for cp, old_gid in kept:
        s, e = glyph_bytes(font, old_gid)
        new_dsc.append((len(out_bitmap),) + dsc[old_gid][1:])
        out_bitmap += font['bitmap'][s:e]

    old_bitmap_len = len(font['bitmap'])

    # ---- 3. 拼文件
    out = io.StringIO()
    W = out.write

    W(font['header'])
    W('static LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n')
    for i, (cp, deg) in enumerate(kept):
        d = new_dsc[i + 1]
        size = (new_dsc[i + 2][0] if i + 2 < len(new_dsc) else len(out_bitmap)) - d[0]
        W('    /* U+%04X "%s" */\n' % (cp, _c_char_comment(cp)))
        chunk = out_bitmap[d[0]:d[0] + size]
        for k in range(0, len(chunk), 16):
            line = chunk[k:k + 16]
            W('    ' + ', '.join('0x%02x' % b for b in line) + ',\n')
        # 最后一行去掉尾逗号，贴近 LVGL 原格式（非必需，纯排版）
    # 收尾：把最后一条的结尾逗号换成无逗号形式不好处理，统一保留逗号也合法
    W('};\n\n')

    W('/*---------------------*/\n')
    W('/*  GLYPH DESCRIPTION  */\n')
    W('/*---------------------*/\n\n')
    W('static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n')
    W('    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, '
      '.ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,\n')
    for i, (bi, adv, bw, bh, ox, oy) in enumerate(new_dsc[1:], 1):
        W('    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, '
          '.ofs_x = %d, .ofs_y = %d},\n' % (bi, adv, bw, bh, ox, oy))
    W('};\n\n')

    # ---- cmaps：单个 SPARSE_TINY，glyph_id = 1 + unicode_list 下标
    base = kept[0][0]
    W('/*---------------------*/\n')
    W('/*  CHARACTER MAPPING  */\n')
    W('/*---------------------*/\n\n')
    W('static const uint16_t unicode_list_0[] = {\n')
    for k in range(0, len(kept), 8):
        W('    ' + ', '.join('0x%04x' % (cp - base) for cp, _ in kept[k:k + 8]) + ',\n')
    W('};\n\n')
    W('/*Collect the unicode lists and glyph_id offsets*/\n')
    W('static const lv_font_fmt_txt_cmap_t cmaps[] =\n{\n')
    W('    {\n')
    W('        .range_start = %d, .range_length = %d, .glyph_id_start = 1,\n'
      % (base, kept[-1][0] - base + 1))
    W('        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, '
      '.list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n' % len(kept))
    W('    }\n};\n\n')

    # ---- kerning：只重建 gid -> class 的映射数组，class 值矩阵原样保留
    if font['has_kern']:
        W('/*-----------------*/\n')
        W('/*    KERNING      */\n')
        W('/*-----------------*/\n\n')
        remap = [0] + [old_gid for _, old_gid in kept]
        for arr, comment in (('kern_left_class_mapping', 'Map glyph_ids to kern left classes'),
                             ('kern_right_class_mapping', 'Map glyph_ids to kern right classes')):
            src = font[arr]
            W('/*%s*/\n' % comment)
            W('static const uint8_t %s[] =\n{\n' % arr)
            vals = [src[g] if g < len(src) else 0 for g in remap]
            for k in range(0, len(vals), 8):
                W('    ' + ', '.join(str(v) for v in vals[k:k + 8]) + ',\n')
            W('};\n\n')
        W('/*Kern values between classes*/\n')
        W('static const int8_t kern_class_values[] =\n{\n')
        vals = font['kern_class_values']
        for k in range(0, len(vals), 8):
            W('    ' + ', '.join(str(v) for v in vals[k:k + 8]) + ',\n')
        W('};\n\n')
        W("/*Collect the kern class' data in one place*/\n")
        W('static const lv_font_fmt_txt_kern_classes_t kern_classes =\n{\n')
        W('    .class_pair_values   = kern_class_values,\n')
        W('    .left_class_mapping  = kern_left_class_mapping,\n')
        W('    .right_class_mapping = kern_right_class_mapping,\n')
        W('    .left_class_cnt      = %d,\n' % font['left_class_cnt'])
        W('    .right_class_cnt     = %d,\n' % font['right_class_cnt'])
        W('};\n\n')

    W('/*--------------------*/\n')
    W('/*  ALL CUSTOM DATA   */\n')
    W('/*--------------------*/\n\n')
    W('#if LVGL_VERSION_MAJOR == 8\n')
    W('/*Store all the custom data of the font*/\n')
    W('static  lv_font_fmt_txt_glyph_cache_t cache;\n')
    W('#endif\n\n')
    W('#if LVGL_VERSION_MAJOR >= 8\n')
    W('static const lv_font_fmt_txt_dsc_t font_dsc = {\n')
    W('#else\n')
    W('static lv_font_fmt_txt_dsc_t font_dsc = {\n')
    W('#endif\n')
    W('    .glyph_bitmap = glyph_bitmap,\n')
    W('    .glyph_dsc = glyph_dsc,\n')
    W('    .cmaps = cmaps,\n')
    W('    .kern_dsc = %s,\n' % ('&kern_classes' if font['has_kern'] else 'NULL'))
    W('    .kern_scale = %d,\n' % font['kern_scale'])
    W('    .cmap_num = 1,\n')
    W('    .bpp = %d,\n' % bpp)
    W('    .kern_classes = %d,\n' % (1 if font['has_kern'] else 0))
    W('    .bitmap_format = 0,\n')
    W('#if LVGL_VERSION_MAJOR == 8\n')
    W('    .cache = &cache\n')
    W('#endif\n')
    W('    .stride = %d\n' % stride)
    W('};\n\n')
    if FALLBACK_FONT:
        W('extern const lv_font_t %s;\n\n\n' % FALLBACK_FONT)
        W(font['public_font']
          .replace('&lv_font_montserrat_14_aligned', '&%s' % FALLBACK_FONT)
          .replace('lv_font_montserrat_14_aligned', FALLBACK_FONT))
    else:
        # fallback 置 NULL：montserrat_14 不再被引用，链接器会把它整个丢掉
        W('\n\n')
        W(font['public_font']
          .replace('&lv_font_montserrat_14_aligned', 'NULL'))

    src = out.getvalue()
    # 清掉可能出现的空行堆积（保持可读性，不影响编译）
    src = re.sub(r'\n{4,}', '\n\n\n', src)

    return src, len(kept), old_bitmap_len, len(out_bitmap)


def main():
    if not os.path.isdir(SRC_DIR):
        raise SystemExit('找不到输入目录 %s' % SRC_DIR)
    os.makedirs(DST_DIR, exist_ok=True)

    print('%-34s %6s %10s %10s %8s' % ('字体', '字形', '原位图', '子集位图', '降幅'))
    print('-' * 74)
    tot_old = tot_new = 0
    for name in FONT_TEXT:
        src_path = os.path.join(SRC_DIR, name + '.c')
        if not os.path.isfile(src_path):
            raise SystemExit('缺少源文件 %s' % src_path)
        font = parse_font(src_path)
        text, nglyph, old_len, new_len = emit(name, font)
        with open(os.path.join(DST_DIR, name + '.c'), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
        tot_old += old_len
        tot_new += new_len
        print('%-34s %6d %10d %10d %7.1fx'
              % (name, nglyph, old_len, new_len, old_len / max(new_len, 1)))
    print('-' * 74)
    print('%-34s %6s %10d %10d %7.1fx'
          % ('合计', '', tot_old, tot_new, tot_old / max(tot_new, 1)))
    print()
    print('位图省下 %d 字节 (%.1f KB)，输出到 UI/assets/fonts/'
          % (tot_old - tot_new, (tot_old - tot_new) / 1024.0))


if __name__ == '__main__':
    main()
