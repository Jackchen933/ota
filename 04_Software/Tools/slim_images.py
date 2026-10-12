#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
图片瘦身 —— 把 GUI Guider 导出的 RGB565A8 图片按需降成 RGB565。

原理
----
LVGL 9 的 RGB565A8 布局是 [w*h*2 字节 RGB565][w*h 字节 alpha]，alpha 紧跟在
颜色平面后面。所以"去掉 alpha"就是把数组尾部 w*h 个字节切掉，再把
`.header.cf` 从 LV_COLOR_FORMAT_RGB565A8 改成 LV_COLOR_FORMAT_RGB565：

  - .header.stride 不用动 —— 它描述的是**颜色**平面的行跨度，两种格式都是 w*2
  - .data_size = sizeof(..._map) 会自动跟着数组变小

符号名里的 "RGB565A8" 后缀**故意不改** —— 改了就得同步改 gg_screen*.c 里的引用，
而这些文件是 GUI Guider 原样导出的、不该手改。格式由 .header.cf 决定，不由名字决定。

哪些图能砍
----------
判据只有一个：**全不透明像素占比 == 100%**，此时砍掉 alpha 视觉上零变化。
逐张统计过（见 Tools/README.md 里的表）：

  Ellipse_61x53      100.0% 全不透明        → 砍，无损
  biaopan1_183x153   100.0% 全不透明        → 砍，无损
  MDLBG_240x280       99.4% 全不透明        → 砍，代价见下
  其余 19 张         40~80% 是**全透明**   → 绝不能砍，砍了整个图标变黑方块

MDLBG 单独说明
--------------
它那 408 个非全不透明像素（0.61%）**全部集中在四个角**，从边缘 38 个递减到
y=18 归零 —— 就是一个圆角遮罩。其中 292 个是全透明、RGB 恰好是 0x0000（纯黑），
另外 116 个是圆角弧上的抗锯齿像素、RGB 偏红。

所以只要**把 screen_1 的屏幕背景色设成 0x000000**，那 292 个像素去 alpha 后
和原来逐像素等价（黑的画在黑的上）。剩下 116 个会从"半透明红叠在黑底上"变成
"纯红"，亮一点点，在圆弧上基本看不出来。

这个背景色改动不在本脚本里做，要手工改 gg_screen_1.c —— 见 Tools/README.md。

用法
----
    python Tools/slim_images.py

输入  generated/assets/images/*.c   （GUI Guider 原样导出）
输出  UI/assets/images/*.c          （本脚本生成，可重复运行）
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, 'generated', 'assets', 'images')
DST_DIR = os.path.join(ROOT, 'UI', 'assets', 'images')
FONT_SRC = os.path.join(ROOT, 'generated', 'assets', 'fonts', 'gg_font.h')
FONT_DST = os.path.join(ROOT, 'UI', 'assets', 'fonts', 'gg_font.h')

# 可以安全去掉 alpha 的图片（全不透明像素占比 100%，或 MDLBG 那种圆角遮罩）
DROP_ALPHA = {
    'Ellipse_61x53_RGB565A8',
    'biaopan1_183x153_RGB565A8',
    'MDLBG_240x280_RGB565A8',
}

# 需要降分辨率的图片：宽高各减半（2x2 盒滤波），显示时由代码放大回去。
# 022 移植后 Flash 依然紧张，把四张"大件"全部减半（奇数尺寸丢边缘 1px）：
#   MDLBG    240x280 → 120x140   134,400B →  33,600B（去 alpha 后）
#   biaopan1 183x153 →  91x76     55,998B →  13,832B（去 alpha 后）
#   ZNZBG    100x100 →  50x50     30,000B →   7,500B（保留 alpha）
#   Ellipse   61x53  →  30x26      6,466B →   1,560B（去 alpha 后）
# 合计省约 172KB。显示端用 lv_image_set_scale(512) + antialias 放大回原尺寸，
# 盒滤波 + 双线性放大后背景会柔和、图标会柔边，用户已确认接受。
#
# 注意：本脚本只负责出图，gg_screen*.c 里的引用要手工跟着改（这些文件本来就
# 因背景色/减半被手改过），对每张减半的图，把 &旧符号名 换成 &新符号名，并在
# lv_image_set_src 之后加：
#     lv_image_set_pivot(img, 0, 0);      /* 从左上角放大 */
#     lv_image_set_scale(img, 512);       /* 256 = 100%，512 = 2x；奇数尺寸会有 1px 差 */
#     lv_image_set_antialias(img, true);  /* 双线性插值必需 */
HALVE = {
    'MDLBG_240x280_RGB565A8',
    'biaopan1_183x153_RGB565A8',
    'ZNZBG_100x100_RGB565A8',
    'Ellipse_61x53_RGB565A8',
}

MAP_RE = re.compile(r'(\w+_map)\[\]\s*=\s*\{(.*?)\};', re.S)


def fmt_bytes(data):
    """按 GUI Guider 的排版输出：每行 16 字节，逗号后不空格。"""
    lines = []
    for k in range(0, len(data), 16):
        lines.append('    ' + ','.join('0x%02x' % b for b in data[k:k + 16]) + ',')
    # 最后一行去掉尾逗号（合法但更好看）
    if lines:
        lines[-1] = lines[-1].rstrip(',')
    return '\n'.join(lines)


def halve_rgb565(data, w, h):
    """RGB565 平面 2x2 盒滤波减半。

    奇数宽高时丢掉最后一行/列（边缘 1px）。每个 2x2 块四个像素在 R5/G6/B5
    通道内取平均（四舍五入）。盒滤波比单点抽样抗锯齿，放大回去观感更好。"""
    w2, h2 = w // 2, h // 2
    out = bytearray()
    for y in range(h2):
        ya, yb = y * 2, min(y * 2 + 1, h - 1)
        rows = [data[ya * w * 2:(ya + 1) * w * 2], data[yb * w * 2:(yb + 1) * w * 2]]
        for x in range(w2):
            xa, xb = x * 2, min(x * 2 + 1, w - 1)
            px = []
            for row in rows:
                for xx in (xa, xb):
                    o = xx * 2
                    px.append(row[o] | (row[o + 1] << 8))
            n = len(px)
            r = (sum((p >> 11) & 0x1F for p in px) + n // 2) // n
            g = (sum((p >> 5) & 0x3F for p in px) + n // 2) // n
            b = (sum(p & 0x1F for p in px) + n // 2) // n
            v = (r << 11) | (g << 5) | b
            out += bytes((v & 0xFF, v >> 8))
    return bytes(out), w2, h2


def halve_a8(data, w, h):
    """A8 alpha 平面 2x2 盒滤波减半。单字节数据直接平均，奇数处理同上。"""
    w2, h2 = w // 2, h // 2
    out = bytearray()
    for y in range(h2):
        ya, yb = y * 2, min(y * 2 + 1, h - 1)
        for x in range(w2):
            xa, xb = x * 2, min(x * 2 + 1, w - 1)
            px = (data[ya * w + xa], data[ya * w + xb], data[yb * w + xa], data[yb * w + xb])
            n = len(px)
            out.append((sum(px) + n // 2) // n)
    return bytes(out), w2, h2


def rename_dims(sym, w2, h2):
    """把符号名里嵌的尺寸段换成减半后的，如 MDLBG_240x280 → MDLBG_120x140。"""
    return re.sub(r'_\d+x\d+_', '_%dx%d_' % (w2, h2), sym, count=1)


def main():
    if not os.path.isdir(SRC_DIR):
        raise SystemExit('找不到输入目录 %s' % SRC_DIR)
    os.makedirs(DST_DIR, exist_ok=True)

    print('%-38s %9s %9s %8s  %s' % ('图片', '原字节', '新字节', '省下', '处理'))
    print('-' * 84)

    tot_old = tot_new = 0
    produced = set()    # 本次生成的文件名，用来清理上次跑剩下的旧输出
    hdr_repl = {}       # 减半图片的符号改名，同步 patch 到 gg_image.h
    files = sorted(f for f in os.listdir(SRC_DIR) if f.endswith('.c'))
    for fn in files:
        path = os.path.join(SRC_DIR, fn)
        src = open(path, encoding='utf-8', errors='replace').read()
        name = fn[:-2]

        m = MAP_RE.search(src)
        if m is None:
            raise SystemExit('%s: 找不到 _map 数组' % fn)
        data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(2)))

        w = int(re.search(r'\.header\.w = (\d+)', src).group(1))
        h = int(re.search(r'\.header\.h = (\d+)', src).group(1))
        cf = re.search(r'\.header\.cf = (\w+)', src).group(1)

        if name in DROP_ALPHA:
            if cf != 'LV_COLOR_FORMAT_RGB565A8':
                raise SystemExit('%s: 预期 RGB565A8，实际 %s' % (fn, cf))
            n_rgb = w * h * 2
            if len(data) != w * h * 3:
                raise SystemExit('%s: 数据长度 %d != w*h*3 (%d)，布局假设不成立'
                                 % (fn, len(data), w * h * 3))
            new_data = data[:n_rgb]
            src = MAP_RE.sub(lambda mm: '%s[] = {\n%s\n};' % (mm.group(1), fmt_bytes(new_data)), src, count=1)
            src = src.replace('.header.cf = LV_COLOR_FORMAT_RGB565A8',
                              '.header.cf = LV_COLOR_FORMAT_RGB565', 1)
            # 在文件顶部插一行说明，免得以后有人纳闷名字和格式对不上
            src = ('/* 已由 Tools/slim_images.py 去掉 alpha：原 RGB565A8 → RGB565。\n'
                   ' * 符号名保留 A8 后缀是为了不改动 GUI Guider 导出的 gg_screen*.c。\n'
                   ' * 格式以 .header.cf 为准。 */\n') + src
            action = '去 alpha'
        else:
            new_data = data
            action = '原样保留'

        if name in HALVE:
            # 两种输入都支持：
            #   纯 RGB565（同时进了 DROP_ALPHA）——只减半颜色平面
            #   RGB565A8（保留 alpha 的图标）——颜色平面和 alpha 平面分别减半，
            #     再按 LVGL 的 [rgb][alpha] 布局拼回去
            if len(new_data) == w * h * 2:
                rgb = new_data
                alpha = None
            elif len(new_data) == w * h * 3:
                rgb = new_data[:w * h * 2]
                alpha = new_data[w * h * 2:]
            else:
                raise SystemExit('%s: 数据长度 %d 不是 RGB565/RGB565A8 平面'
                                 % (fn, len(new_data)))
            rgb, w2, h2 = halve_rgb565(rgb, w, h)
            if alpha is not None:
                alpha, _, _ = halve_a8(alpha, w, h)
                new_data = rgb + alpha
            else:
                new_data = rgb
            new_name = rename_dims(name, w2, h2)
            # 符号名、宽高、stride（= w*2）全部跟着改，保证 .header 自洽
            src = src.replace(name, new_name)
            src = re.sub(r'\.header\.w = \d+', '.header.w = %d' % w2, src, count=1)
            src = re.sub(r'\.header\.h = \d+', '.header.h = %d' % h2, src, count=1)
            src = re.sub(r'\.header\.stride = \d+', '.header.stride = %d' % (w2 * 2),
                         src, count=1)
            src = ('/* 已由 Tools/slim_images.py 减半存储（%dx%d -> %dx%d，2x2 盒滤波）。\n'
                   ' * 显示端要用 lv_image_set_scale(512) + pivot(0,0) 放大回满屏，\n'
                   ' * 见 gg_screen_1.c。 */\n' % (w // 2, h // 2, w2, h2)) + src
            action = '去 alpha + 减半'
            # map 数组也要重写成减半后的数据（上面去 alpha 写过一次全尺寸的）
            src = MAP_RE.sub(lambda mm: '%s[] = {\n%s\n};' % (mm.group(1), fmt_bytes(new_data)),
                             src, count=1)
            hdr_repl[name] = new_name
        else:
            new_data = data
            action = '原样保留'

        out_fn = rename_dims(name, w // 2, h // 2) + '.c' if name in HALVE else fn
        produced.add(out_fn)
        out_path = os.path.join(DST_DIR, out_fn)
        with open(out_path, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(src)

        tot_old += len(data)
        tot_new += len(new_data)
        print('%-38s %9d %9d %8d  %s' % (name, len(data), len(new_data),
                                         len(data) - len(new_data), action))

    print('-' * 84)
    print('%-38s %9d %9d %8d' % ('合计', tot_old, tot_new, tot_old - tot_new))
    print()
    print('省下 %d 字节 (%.1f KB)' % (tot_old - tot_new, (tot_old - tot_new) / 1024.0))

    # 清理上次运行留下的、本次不再生成的输出（比如减半改名前的旧文件）
    for f in os.listdir(DST_DIR):
        if f.endswith('.c') and f not in produced:
            os.remove(os.path.join(DST_DIR, f))
            print('已删除过时输出 %s' % f)

    # gg_image.h / gg_font.h 一起搬到 UI/assets 下（gg_font.h 由 slim_fonts.py 之外
    # 单独处理，因为它只是声明，不需要改）
    for s, d in ((os.path.join(SRC_DIR, 'gg_image.h'),
                  os.path.join(DST_DIR, 'gg_image.h')),
                 (FONT_SRC, FONT_DST)):
        if os.path.isfile(s):
            with open(s, encoding='utf-8', errors='replace') as fh:
                content = fh.read()
            for old, new in hdr_repl.items():
                content = content.replace(old, new)
            with open(d, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(content)
            print('已复制 %s' % os.path.basename(s))


if __name__ == '__main__':
    main()
