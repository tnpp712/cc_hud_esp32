#!/usr/bin/env python3
"""生成 Codex HUD 自绘终端图标、工具图标及光标动画，不依赖外部图片。"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
ACCENT = "#63e6be"
WHITE = "#effaf5"


def render(kind, frame=0):
    im = Image.new("RGB", (160, 160), "black")
    d = ImageDraw.Draw(im)
    def line(points, color=ACCENT, width=2):
        d.line([(x*SCALE, y*SCALE) for x, y in points], fill=color, width=width*SCALE, joint="curve")
    def box(rect, color=ACCENT, radius=3):
        d.rounded_rectangle(tuple(v*SCALE for v in rect), radius=radius*SCALE, outline=color, width=2*SCALE)
    if kind in ("logo", "terminal"):
        box((3, 5, 37, 35), WHITE)
        line([(9, 13), (16, 20), (9, 27)])
        line([(22, 27), (31, 27)], ACCENT if frame < 8 else "#224c3e")
        if kind == "terminal":
            line([(6 + frame*2, 33), (10 + frame*2, 33)], WHITE, 1)
    elif kind == "edit":
        line([(8, 28), (10, 21), (27, 4), (34, 11), (17, 28), (8, 28), (11, 22)], WHITE)
        line([(6, 35), (34, 35)])
    elif kind in ("read", "write"):
        box((9, 4, 31, 36), WHITE)
        for y in (12, 18, 24): line([(14,y),(26,y)])
        if kind == "write": line([(20, 26), (20, 34)]);line([(16,30),(24,30)])
    elif kind in ("search", "files"):
        d.ellipse((7*SCALE, 5*SCALE, 28*SCALE, 26*SCALE), outline=WHITE, width=2*SCALE)
        line([(25,24),(35,34)])
        if kind == "files":line([(13,15),(22,15)])
    elif kind == "agents":
        for x,y in ((20,7),(9,29),(31,29)):box((x-5,y-4,x+5,y+5),WHITE,2)
        line([(20,12),(20,19),(9,19),(9,24)]);line([(20,19),(31,19),(31,24)])
    elif kind == "web":
        d.ellipse((4*SCALE,4*SCALE,36*SCALE,36*SCALE),outline=WHITE,width=2*SCALE)
        d.ellipse((13*SCALE,4*SCALE,27*SCALE,36*SCALE),outline=ACCENT,width=2*SCALE)
        line([(5,15),(35,15)]);line([(5,25),(35,25)])
    elif kind == "skill":
        line([(23,3),(9,23),(19,23),(16,37),(32,16),(22,16),(23,3)],WHITE)
    else:
        box((11,11,29,29),WHITE)
        for v in (15,25):
            line([(v,4),(v,11)]);line([(v,29),(v,36)])
            line([(4,v),(11,v)]);line([(29,v),(36,v)])
    return im.resize((40,40),Image.Resampling.LANCZOS)


def pixels(im):
    values=[((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r,g,b in im.getdata()]
    return "\n".join("    "+",".join(f"0x{x:04x}" for x in values[i:i+40])+"," for i in range(0,len(values),40))


def main():
    header="// 由 tools/generate_codex_assets.py 生成。\n#pragma once\n#include <cstdint>\n"
    (ROOT/'src/logo_brand.h').write_text(header+'constexpr int kLogoW=40, kLogoH=40;\nconstexpr uint16_t kLogo[1600]={\n'+pixels(render('logo'))+'\n};\n')
    frames=[render('terminal',n) for n in range(12)]
    (ROOT/'src/codex_frames.h').write_text(header+'constexpr int kCodexFrameW=40, kCodexFrameH=40, kCodexFrameCount=12;\nconstexpr uint16_t kCodexFrames[12][1600]={\n'+',\n'.join('{\n'+pixels(im)+'\n}' for im in frames)+'\n};\n')
    kinds=['terminal','edit','read','write','search','files','agents','web','skill','mcp']
    icons=[render(k) for k in kinds]
    old=(ROOT/'src/tool_icons.h').read_text()
    mapping=old[old.index('struct IconMapping'):]
    for name,idx in [('exec_command',0),('write_stdin',0),('apply_patch',1),('read_file',2),('web',7),('spawn_agent',6)]:
        if f'{{"{name}"' not in mapping:mapping=mapping.replace('constexpr IconMapping kIconMap[] = {',f'constexpr IconMapping kIconMap[] = {{\n    {{"{name}", {idx}}},')
    (ROOT/'src/tool_icons.h').write_text(header+'constexpr int kIconW=40, kIconH=40, kIconCount=10;\nconstexpr uint16_t kIcons[10][1600]={\n'+',\n'.join('{\n'+pixels(im)+'\n}' for im in icons)+'\n};\n'+mapping)
    preview=Image.new('RGB',(440,80),'black')
    for i,im in enumerate([render('logo')]+icons):preview.paste(im.resize((40,40)),(i*40,20))
    preview.save(ROOT/'tools/codex-preview.png')


if __name__=='__main__':main()
