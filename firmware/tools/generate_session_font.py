#!/usr/bin/env python3
"""生成小字项目行与步骤字体，复用已有大字字体的中文字符集合。"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
header = (root / 'src/font_cn_20.c').read_text().splitlines()[3]
symbols = header.split('--symbols ', 1)[1] + '强度上下文已用待回答本轮最近工具会话忙碌，。！？；：“”‘’（）【】《》、…—→'
# 同步标题字库的常用标点，避免公开进度和结果卡出现空框。
subprocess.run(['npx', '--offline', 'lv_font_conv', '--font',
    '/System/Library/Fonts/Supplemental/Arial Unicode.ttf', '--size', '20',
    '--bpp', '4', '--format', 'lvgl', '--no-compress', '-o', str(root / 'src/font_cn_20.c'),
    '-r', '0x20-0x7E', '-r', '0xB0', '-r', '0xB7', '-r', '0x2103', '-r', '0x2109',
    '--symbols', '一' + ''.join(sorted(set(symbols)-{'一'}))], check=True)
subprocess.run(['npx', '--offline', 'lv_font_conv', '--font',
    '/System/Library/Fonts/Supplemental/Arial Unicode.ttf', '--size', '16',
    '--bpp', '4', '--format', 'lvgl', '-o', str(root / 'src/font_cn_16.c'),
    '-r', '0x20-0x7E', '-r', '0xB7', '--symbols', '一' + ''.join(sorted(set(symbols)-{'一'}))], check=True)
