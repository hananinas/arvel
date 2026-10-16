#!/usr/bin/env python3
"""Generate the Arvel LVGL assets from the bundled design reference.

Inputs : "Arvel Buddy — ESP32 Display.html" (the self-unpacking design bundle).
Outputs: components/ui_bsp/arvel_art.c      scene layers, animation tracks,
                                            thumbnails, character, icons and
                                            tool-call and weather icons
         components/ui_bsp/fonts/*.c         Inter fonts at the design's sizes

Every bitmap is rendered by headless Chrome from the design's own SVG, so the
device shows the reference pixels.  Scene layers and the full-colour tool icons
are stored as PNG and decoded into PSRAM when needed; the other icons are
stored as 8-bit alpha masks.

Requirements: google-chrome, Pillow, fontTools (+brotli for woff2) and
`lv_font_conv` (npx lv_font_conv, or LV_FONT_CONV=/path/to/lv_font_conv).
Usage: python3 scripts/generate_arvel_assets.py [design.html]
"""
import base64
import gzip
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_DIR = os.path.join(ROOT, 'components', 'ui_bsp')
SVG_NS = 'http://www.w3.org/2000/svg'
ET.register_namespace('', SVG_NS)

# Device scene ids (persisted in settings) -> design scene keys and picker names.
SCENES = [
    ('meadow', 'meadow', 'Meadow', False),
    ('night', 'night', 'Night', True),
    ('workshop', 'workshop', 'Workshop', False),
    ('dunes', 'dunes', 'Dunes', False),
    ('rainy-window', 'rain', 'Rainy', True),
    ('snow', 'snow', 'Snow', False),
    ('autumn', 'autumn', 'Autumn', False),
    ('blossom', 'blossom', 'Blossom', False),
    ('rooftop', 'city', 'Rooftop', True),
    ('aurora', 'aurora', 'Aurora', True),
    ('library', 'library', 'Library', False),
    ('campfire', 'campfire', 'Campfire', True),
]

# Lucide icons used by the screen, as (name, size, stroke width, svg body).
ICONS = [
    ('wifi', 13, 2.4, '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.859a10 10 0 0 1 14 0"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>'),
    ('settings', 14, 2.2, '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>'),
    ('image', 19, 2.0, '<rect width="18" height="18" x="3" y="3" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"/>'),
    ('chat', 19, 2.0, '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>'),
    ('mic', 20, 2.0, '<path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><line x1="12" x2="12" y1="19" y2="22"/>'),
    ('stop', 16, 0, '<rect x="5" y="5" width="14" height="14" rx="3" fill="#000" stroke="none"/>'),
    ('shield', 13, 2.2, '<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="m9 12 2 2 4-4"/>'),
    ('speaker', 15, 2.2, '<path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z"/>'),
    ('wave1', 15, 2.2, '<path d="M16 9a5 5 0 0 1 0 6"/>'),
    ('wave2', 15, 2.2, '<path d="M19.364 18.364a9 9 0 0 0 0-12.728"/>'),
    ('chevron_up', 15, 2.4, '<path d="m18 15-6-6-6 6"/>'),
    ('chevron_down', 15, 2.4, '<path d="m6 9 6 6 6-6"/>'),
    ('chevron_left', 15, 2.4, '<path d="m15 18-6-6 6-6"/>'),
    ('chevron_right', 15, 2.4, '<path d="m9 18 6-6-6-6"/>'),
    ('chevron_right_small', 14, 2.2, '<path d="m9 18 6-6-6-6"/>'),
    ('check', 14, 3.0, '<path d="M20 6 9 17l-5-5"/>'),
    ('check_small', 11, 3.6, '<path d="M20 6 9 17l-5-5"/>'),
    ('close', 12, 2.8, '<path d="M18 6 6 18M6 6l12 12"/>'),
    ('bell', 30, 2.0, '<path d="M10.268 21a2 2 0 0 0 3.464 0"/><path d="M3.262 15.326A1 1 0 0 0 4 17h16a1 1 0 0 0 .74-1.673C19.41 13.956 18 12.499 18 8A6 6 0 0 0 6 8c0 4.499-1.411 5.956-2.738 7.326"/>'),
    # The speech bubble's tail: a 14px square with 3px corners rotated 45°.
    ('tail', 24, 0, '<rect x="5" y="5" width="14" height="14" rx="3" transform="rotate(45 12 12)" fill="#000" stroke="none"/>'),
]

# Tool-call icon kinds in the design's ToolIcon component, and the sizes the
# screen draws them at (22px in the thinking list, 20px in chat chips).
TOOL_KINDS = ['search', 'read', 'run', 'edit', 'mcp', 'git', 'approve', 'harness',
              'timer', 'notes', 'calc', 'weather']
TOOL_SIZES = [22, 20]
# Weather icon kinds in the design's WxIcon component, at the weather page's
# current-conditions (54px) and hourly (26px) sizes.
WEATHER_KINDS = ['sun', 'partly', 'cloud', 'rain']
WEATHER_SIZES = [54, 26]

# (C name, size px, weight) — every text style used by the design. Fractional
# design sizes (12.5px, 11.5px) round down to the nearest whole pixel.
FONTS = [
    ('arvel_font_11_500', 11, 500), ('arvel_font_11_700', 11, 700),
    ('arvel_font_12_400', 12, 400), ('arvel_font_12_500', 12, 500),
    ('arvel_font_12_600', 12, 600), ('arvel_font_12_650', 12, 650),
    ('arvel_font_12_700', 12, 700), ('arvel_font_11_650', 11, 650),
    ('arvel_font_13_400', 13, 400), ('arvel_font_13_500', 13, 500),
    ('arvel_font_13_550', 13, 550), ('arvel_font_13_650', 13, 650),
    ('arvel_font_14_400', 14, 400), ('arvel_font_15_600', 15, 600),
    ('arvel_font_15_650', 15, 650), ('arvel_font_17_550', 17, 550),
    ('arvel_font_17_650', 17, 650), ('arvel_font_18_550', 18, 550),
    ('arvel_font_21_650', 21, 650), ('arvel_font_14_650', 14, 650),
]
# Styles that only draw the tool pages' own strings (tags, units, round and
# weather details, done titles) and numerals carry just the glyphs they need.
UI_RANGES = '0x20-0x7E,0xB0,0xB7,0x2014,0x2019,0x2026,0x2212'
SUBSET_FONTS = [('arvel_font_10_400', 10, 400, UI_RANGES), ('arvel_font_10_600', 10, 600, UI_RANGES),
                ('arvel_font_11_400', 11, 400, UI_RANGES), ('arvel_font_18_600', 18, 600, UI_RANGES),
                ('arvel_font_19_650', 19, 650, UI_RANGES),
                ('arvel_font_22_700', 22, 700, '0x20,0x2B-0x3A,0x2212'),
                ('arvel_font_34_700', 34, 700, '0x20,0x2B-0x39,0x45,0x65,0xB0,0x2212')]
# Inter has no italic in the design bundle; browsers synthesize it by skewing
# the upright glyphs a quarter of their height, so the device does the same.
ITALIC_FONTS = [('arvel_font_12_400i', 12, 400)]
ITALIC_SKEW = 0.25
# Tool targets use the design's ui-monospace stack; Noto Sans Mono (SIL OFL 1.1)
# is what that stack resolves to on Linux.
MONO_FONT = '/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf'
MONO_FONTS = [('arvel_mono_12', 12), ('arvel_mono_11', 11)]
LATIN_RANGES = '0x20-0x7E,0xA0-0xFF,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026,0x2212'
MONO_EXTRA_RANGES = '0x2192'  # → in calculator conversions
LATIN_EXT_RANGES = '0x100-0x17F'
# The design's Inter subsets lack ■ ("Tap ■ when you're done"); browsers fall back
# to another font for it, so take it from a system Inter build when available.
SYMBOL_FONT = '/usr/share/fonts/rsms-inter-fonts/Inter-Regular.ttf'
SYMBOL_RANGES = '0x25A0'

MARGIN = 72  # off-screen margin captured around moving scene sprites


# --------------------------------------------------------------- bundle input
def unpack(html):
    """Return {uuid: (mime, bytes)} and the template of a bundler page."""
    def grab(kind):
        m = re.search(r'<script type="__bundler/' + kind + r'">(.*?)</script>', html, re.S)
        return m.group(1) if m else None
    assets = {}
    for uuid, entry in json.loads(grab('manifest')).items():
        data = base64.b64decode(entry['data'])
        if entry.get('compressed'):
            data = gzip.decompress(data)
        assets[uuid] = (entry['mime'], data)
    return assets, json.loads(grab('template'))


def decamel(text):
    def repl(m):
        parts = m.group(1).split('-')
        return parts[0] + ''.join(p.capitalize() for p in parts[1:]) + '='
    return re.sub(r'sc-camel-([a-z\-]+)=', repl, text)


def load_design(path):
    root_assets, _ = unpack(open(path, encoding='utf-8').read())
    screen = scene_lib = tool_icon = weather_icon = None
    font = None
    for mime, data in root_assets.values():
        if mime != 'text/html':
            continue
        assets, template = unpack(data.decode('utf-8'))
        if '<title>Arvel screen</title>' in template and screen is None:
            screen = decamel(template)
            # The latin Inter subset is the last font-face in the screen bundle.
            fonts = {}
            for subset in ('latin', 'latin-ext'):
                urls = re.findall(r'/\* ' + subset + r' \*/\s*@font-face\s*\{[^}]*?url\("([0-9a-f-]+)"\)', template)
                if urls:
                    fonts[subset] = assets[urls[-1]][1]
            if len(fonts) == 2:
                font = fonts
        if '<title>Arvel scene</title>' in template:
            scene_lib = decamel(template)
        if '<title>Tool icon</title>' in template:
            tool_icon = decamel(template)
        if '<title>Weather icon</title>' in template:
            weather_icon = decamel(template)
    if not (screen and scene_lib and tool_icon and weather_icon and font):
        sys.exit('Design bundle does not contain the screen, scene library, tool and weather icons and Inter font')
    return screen, scene_lib, tool_icon, weather_icon, font


# ------------------------------------------------------------------ rendering
class Renderer:
    """Batch-renders SVG snippets with one headless Chrome screenshot."""

    def __init__(self):
        self.jobs = []

    def add(self, svg, w, h):
        self.jobs.append((svg, w, h))
        return len(self.jobs) - 1

    def run(self):
        width = max(w for _, w, _ in self.jobs) + 8
        y = 0
        placed, parts = [], []
        for svg, w, h in self.jobs:
            placed.append((y, w, h))
            parts.append(f'<div style="position:absolute;left:0;top:{y}px;width:{w}px;height:{h}px">{svg}</div>')
            y += h + 8
        page = ('<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;background:transparent}'
                'svg{display:block}</style></head><body>' + ''.join(parts) + '</body></html>')
        tmp = tempfile.mkdtemp(prefix='arvel_render_')
        try:
            src = os.path.join(tmp, 'page.html')
            out = os.path.join(tmp, 'shot.png')
            open(src, 'w', encoding='utf-8').write(page)
            chrome = shutil.which('google-chrome') or shutil.which('chromium') or 'google-chrome'
            subprocess.run([chrome, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                            '--force-device-scale-factor=1', '--default-background-color=00000000',
                            f'--window-size={width},{y}', f'--screenshot={out}', 'file://' + src],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            shot = Image.open(out).convert('RGBA')
            shot.load()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        return [shot.crop((0, top, w, top + h)) for top, w, h in placed]


def svg_wrap(inner, view, w, h, aspect='none'):
    return (f'<svg xmlns="{SVG_NS}" width="{w}" height="{h}" viewBox="{view}" '
            f'preserveAspectRatio="{aspect}">{inner}</svg>')


def tostr(el):
    return ET.tostring(el, encoding='unicode')


def strip_anims(el):
    for parent in el.iter():
        for child in list(parent):
            if child.tag.split('}')[-1] in ('animate', 'animateTransform'):
                parent.remove(child)
    return el


def anims_of(el):
    return [a for a in el.iter() if a.tag.split('}')[-1] in ('animate', 'animateTransform')]


def seconds(value):
    return float(value.rstrip('s')) if value else 0.0


# ------------------------------------------------------------- scene layering
def flatten(children):
    """Split groups whose descendants animate separately into one group per child."""
    out = []
    for child in children:
        nested = [a for a in anims_of(child) if a not in list(child)]
        if child.tag.split('}')[-1] == 'g' and nested:
            for sub in list(child):
                wrapper = ET.Element(child.tag, dict(child.attrib))
                wrapper.append(sub)
                out.append(wrapper)
        else:
            out.append(child)
    return out


def scene_layers(svg_text, renderer):
    """Split a scene into an opaque base plus static/animated sprites."""
    root = ET.fromstring(svg_text)
    children = flatten(list(root))
    segments, run = [], []
    for child in children:
        if anims_of(child):
            if run:
                segments.append(('static', run))
                run = []
            segments.append(('anim', [child]))
        else:
            run.append(child)
    if run:
        segments.append(('static', run))
    if segments[0][0] != 'static':
        sys.exit('Scene must start with a static background')

    layers = []
    view = f'{-MARGIN} {-MARGIN} {640 + 2 * MARGIN} {172 + 2 * MARGIN}'
    big_w, big_h = 640 + 2 * MARGIN, 172 + 2 * MARGIN
    for index, (kind, elements) in enumerate(segments):
        if index == 0:
            inner = ''.join(tostr(e) for e in elements)
            layers.append({'kind': 'base', 'job': renderer.add(svg_wrap(inner, '0 0 640 172', 640, 172), 640, 172)})
            continue
        if kind == 'static':
            inner = ''.join(tostr(strip_anims(e)) for e in elements)
            layers.append({'kind': 'sprite', 'tracks': [], 'job': renderer.add(svg_wrap(inner, view, big_w, big_h), big_w, big_h)})
            continue
        el = elements[0]
        parents = {c: p for p in el.iter() for c in p}
        tracks, morph = [], None
        for a in anims_of(el):
            target = parents[a]
            attr = a.get('attributeName')
            values = [v.strip() for v in a.get('values').split(';') if v.strip()]
            dur, begin = seconds(a.get('dur')), seconds(a.get('begin'))
            if attr == 'opacity':
                tracks.append(('opa', [round(float(v) * 255) for v in values], dur, begin))
                target.attrib.pop('opacity', None)  # SMIL opacity replaces the static value
            elif attr == 'transform':
                pts = [tuple(float(n) for n in v.replace(',', ' ').split()) for v in values]
                tracks.append(('dx', [round(p[0]) for p in pts], dur, begin))
                tracks.append(('dy', [round(p[1]) for p in pts], dur, begin))
            elif attr == 'cy':
                tracks.append(('dy', [round(float(v) - float(values[0])) for v in values], dur, begin))
                target.set('cy', values[0])
            elif attr == 'd':
                morph = (target, values, dur, begin)
            else:
                sys.exit(f'Unsupported animation attribute {attr}')
        if morph:
            target, values, dur, begin = morph
            target.set('d', values[0])
            alt_d = values[1]
        base_svg = tostr(strip_anims(el))
        layers.append({'kind': 'sprite', 'tracks': tracks,
                       'job': renderer.add(svg_wrap(base_svg, view, big_w, big_h), big_w, big_h)})
        if morph:
            # A;B;A shape morphs become a cross-fade to shape B (opacity 0→1→0).
            target.set('d', alt_d)
            opa = [255 if v == alt_d else 0 for v in values]
            layers.append({'kind': 'sprite', 'tracks': [('opa', opa, dur, begin)],
                           'job': renderer.add(svg_wrap(tostr(el), view, big_w, big_h), big_w, big_h)})
    return layers


def crop_sprite(img):
    box = img.getchannel('A').getbbox()
    if not box:
        return None, 0, 0
    return img.crop(box), box[0] - MARGIN, box[1] - MARGIN


def png_bytes(img, mode):
    buf = io.BytesIO()
    img.convert(mode).save(buf, 'PNG', optimize=True)
    return buf.getvalue()


# ----------------------------------------------------------------- C emitters
def c_bytes(name, data, out):
    out.append(f'static const uint8_t {name}[] = {{')
    for i in range(0, len(data), 20):
        out.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 20]) + ',')
    out.append('};')


def emit_art(scenes, thumbs, character, blink, icons, tools, weather):
    out = ['/* Generated by scripts/generate_arvel_assets.py from the Arvel design. Do not edit. */',
           '#include "arvel_art.h"', '']
    for s_index, scene in enumerate(scenes):
        for l_index, layer in enumerate(scene['layers']):
            c_bytes(f's{s_index}_l{l_index}_png', layer['png'], out)
        for l_index, layer in enumerate(scene['layers']):
            for t_index, (kind, values, dur, begin) in enumerate(layer['tracks']):
                out.append(f'static const int16_t s{s_index}_l{l_index}_t{t_index}[] = {{{", ".join(str(v) for v in values)}}};')
        for l_index, layer in enumerate(scene['layers']):
            if layer['tracks']:
                rows = []
                for t_index, (kind, values, dur, begin) in enumerate(layer['tracks']):
                    rows.append(f'{{ARVEL_TRACK_{kind.upper()}, {len(values)}, {round(dur * 1000)}, {round(begin * 1000)}, s{s_index}_l{l_index}_t{t_index}}}')
                out.append(f'static const arvel_track_t s{s_index}_l{l_index}_tracks[] = {{{", ".join(rows)}}};')
        out.append(f'static const arvel_layer_t s{s_index}_layers[] = {{')
        for l_index, layer in enumerate(scene['layers']):
            tracks = f's{s_index}_l{l_index}_tracks' if layer['tracks'] else 'NULL'
            out.append(f'    {{s{s_index}_l{l_index}_png, sizeof(s{s_index}_l{l_index}_png), {layer["x"]}, {layer["y"]}, '
                       f'{layer["w"]}, {layer["h"]}, {len(layer["tracks"])}, {tracks}}},')
        out.append('};')
        c_bytes(f'thumb{s_index}_png', thumbs[s_index], out)
        out.append('')
    c_bytes('character_png', character, out)
    c_bytes('blink_png', blink['png'], out)
    out.append('')
    out.append('const arvel_scene_t arvel_scenes[ARVEL_SCENE_COUNT] = {')
    for s_index, scene in enumerate(scenes):
        out.append(f'    {{"{scene["id"]}", "{scene["name"]}", {"true" if scene["dark"] else "false"}, '
                   f's{s_index}_layers, {len(scene["layers"])}, thumb{s_index}_png, sizeof(thumb{s_index}_png)}},')
    out.append('};')
    out.append('const arvel_png_t arvel_character_png = {character_png, sizeof(character_png), -4, 18, 170, 170};')
    out.append(f'const arvel_png_t arvel_blink_png = {{blink_png, sizeof(blink_png), {blink["x"]}, {blink["y"]}, {blink["w"]}, {blink["h"]}}};')
    out.append('')
    for kind, pngs in tools:
        for size, data in zip(TOOL_SIZES, pngs):
            c_bytes(f'tool_{kind}_{size}_png', data, out)
    out.append('const arvel_tool_icon_t arvel_tool_icons[ARVEL_TOOL_COUNT] = {')
    for kind, _ in tools:
        sizes = ', '.join(f'{{tool_{kind}_{size}_png, sizeof(tool_{kind}_{size}_png), 0, 0, {size}, {size}}}' for size in TOOL_SIZES)
        out.append(f'    {{"{kind}", {{{sizes}}}}},')
    out.append('};')
    out.append('')
    for kind, pngs in weather:
        for size, data in zip(WEATHER_SIZES, pngs):
            c_bytes(f'weather_{kind}_{size}_png', data, out)
    out.append('const arvel_tool_icon_t arvel_weather_icons[ARVEL_WEATHER_COUNT] = {')
    for kind, _ in weather:
        sizes = ', '.join(f'{{weather_{kind}_{size}_png, sizeof(weather_{kind}_{size}_png), 0, 0, {size}, {size}}}' for size in WEATHER_SIZES)
        out.append(f'    {{"{kind}", {{{sizes}}}}},')
    out.append('};')
    out.append('')
    for name, img in icons:
        alpha = img.getchannel('A').tobytes()
        c_bytes(f'icon_{name}_map', alpha, out)
        out.append(f'const lv_img_dsc_t arvel_icon_{name} = {{.header.always_zero = 0, .header.w = {img.width}, '
                   f'.header.h = {img.height}, .header.cf = LV_IMG_CF_ALPHA_8BIT, .data_size = sizeof(icon_{name}_map), '
                   f'.data = icon_{name}_map}};')
    open(os.path.join(UI_DIR, 'arvel_art.c'), 'w').write('\n'.join(out) + '\n')

    header = ['/* Generated by scripts/generate_arvel_assets.py. Do not edit. */', '#pragma once', '',
              '#include "lvgl.h"', '#include <stdbool.h>', '#include <stddef.h>', '#include <stdint.h>', '',
              '#ifdef __cplusplus', 'extern "C" {', '#endif', '',
              f'#define ARVEL_SCENE_COUNT {len(scenes)}',
              f'#define ARVEL_TOOL_COUNT {len(tools)}',
              f'#define ARVEL_WEATHER_COUNT {len(weather)}', '',
              '/* Keyframe tracks follow SMIL linear values: evenly spaced, looping every period. */',
              'typedef enum { ARVEL_TRACK_OPA, ARVEL_TRACK_DX, ARVEL_TRACK_DY } arvel_track_kind_t;',
              'typedef struct {', '    arvel_track_kind_t kind;', '    uint8_t count;', '    uint16_t period_ms;',
              '    uint16_t begin_ms;', '    const int16_t *values;', '} arvel_track_t;', '',
              '/* PNG layer drawn at (x, y) in screen pixels; layer 0 is the opaque 640x172 base. */',
              'typedef struct {', '    const uint8_t *png;', '    size_t png_size;', '    int16_t x, y, w, h;',
              '    uint8_t track_count;', '    const arvel_track_t *tracks;', '} arvel_layer_t;', '',
              'typedef struct {', '    const char *id;', '    const char *name;', '    bool dark;',
              '    const arvel_layer_t *layers;', '    uint8_t layer_count;', '    const uint8_t *thumb_png;',
              '    size_t thumb_png_size;', '} arvel_scene_t;', '',
              'typedef struct {', '    const uint8_t *png;', '    size_t png_size;', '    int16_t x, y, w, h;', '} arvel_png_t;', '',
              '/* Full-colour icon at two sizes: tool calls in the order of buddy_ui_tool_kind_t at',
              ' * 22px and 20px; weather (sun, partly, cloud, rain) at 54px and 26px. */',
              'typedef struct {', '    const char *kind;', '    arvel_png_t sizes[2];', '} arvel_tool_icon_t;', '',
              'extern const arvel_scene_t arvel_scenes[ARVEL_SCENE_COUNT];',
              'extern const arvel_tool_icon_t arvel_tool_icons[ARVEL_TOOL_COUNT];',
              'extern const arvel_tool_icon_t arvel_weather_icons[ARVEL_WEATHER_COUNT];',
              'extern const arvel_png_t arvel_character_png;', 'extern const arvel_png_t arvel_blink_png;']
    for name, _ in icons:
        header.append(f'extern const lv_img_dsc_t arvel_icon_{name};')
    header += ['', '#ifdef __cplusplus', '}', '#endif', '']
    open(os.path.join(UI_DIR, 'arvel_art.h'), 'w').write('\n'.join(header))


# ---------------------------------------------------------------------- fonts
def skew_font(src, dst):
    """Write an oblique copy of a static TrueType font (synthetic italic)."""
    from fontTools.ttLib import TTFont
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    font = TTFont(src)
    glyphs = font.getGlyphSet()
    glyf = font['glyf']
    skewed = {}
    for name in font.getGlyphOrder():
        rec = DecomposingRecordingPen(glyphs)
        glyphs[name].draw(rec)
        pen = TTGlyphPen(None)
        rec.replay(TransformPen(pen, (1, 0, ITALIC_SKEW, 1, 0, 0)))
        skewed[name] = pen.glyph()
    for name, glyph in skewed.items():
        glyf[name] = glyph
        glyph.recalcBounds(glyf)
    font['hmtx'].metrics = {n: (adv, glyf[n].xMin if glyf[n].numberOfContours else 0)
                            for n, (adv, _) in font['hmtx'].metrics.items()}
    for table in ('gvar', 'HVAR', 'MVAR', 'STAT', 'fvar', 'avar'):
        if table in font:
            del font[table]
    font.save(dst)


def build_fonts(subsets):
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    font_dir = os.path.join(UI_DIR, 'fonts')
    os.makedirs(font_dir, exist_ok=True)
    conv = os.environ.get('LV_FONT_CONV') or shutil.which('lv_font_conv')
    cmd = [conv] if conv else ['npx', '--yes', 'lv_font_conv']
    tmp = tempfile.mkdtemp(prefix='arvel_fonts_')
    try:
        def instance(subset, weight):
            path = os.path.join(tmp, f'inter_{subset}_{weight}.ttf')
            if not os.path.exists(path):
                static = instancer.instantiateVariableFont(TTFont(io.BytesIO(subsets[subset])), {'wght': weight})
                static.flavor = None
                static.save(path)
            return path
        def italic(path):
            out = path[:-4] + '_italic.ttf'
            if not os.path.exists(out):
                skew_font(path, out)
            return out
        def convert(name, size, sources):
            out = os.path.join(font_dir, name + '.c')
            subprocess.run(cmd + ['--bpp', '4', '--size', str(size), '--no-compress', '--format', 'lvgl'] + sources +
                           ['--lv-font-name', name, '--lv-include', 'lvgl.h', '-o', out], check=True)
        for name, size, weight in FONTS:
            sources = ['--font', instance('latin', weight), '-r', LATIN_RANGES,
                       '--font', instance('latin-ext', weight), '-r', LATIN_EXT_RANGES]
            if os.path.exists(SYMBOL_FONT):
                sources += ['--font', SYMBOL_FONT, '-r', SYMBOL_RANGES]
            convert(name, size, sources)
        for name, size, weight, ranges in SUBSET_FONTS:
            convert(name, size, ['--font', instance('latin', weight), '-r', ranges])
        for name, size, weight in ITALIC_FONTS:
            convert(name, size, ['--font', italic(instance('latin', weight)), '-r', LATIN_RANGES,
                                 '--font', italic(instance('latin-ext', weight)), '-r', LATIN_EXT_RANGES])
        mono = os.path.join(tmp, 'mono_400.ttf')
        static = instancer.instantiateVariableFont(TTFont(MONO_FONT), {'wght': 400})
        static.save(mono)
        for name, size in MONO_FONTS:
            out = os.path.join(font_dir, name + '.c')
            subprocess.run(cmd + ['--bpp', '4', '--size', str(size), '--no-compress', '--format', 'lvgl',
                                  '--font', mono, '-r', LATIN_RANGES + ',' + LATIN_EXT_RANGES + ',' + MONO_EXTRA_RANGES,
                                  '--lv-font-name', name, '--lv-include', 'lvgl.h', '-o', out], check=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    header = ['/* Generated by scripts/generate_arvel_assets.py. Inter and Noto Sans Mono,',
              ' * SIL Open Font License 1.1. */',
              '#pragma once', '', '#include "lvgl.h"', '']
    header += [f'LV_FONT_DECLARE({name})' for name, _, _ in FONTS]
    header += [f'LV_FONT_DECLARE({name})' for name, _, _, _ in SUBSET_FONTS]
    header += [f'LV_FONT_DECLARE({name})' for name, _, _ in ITALIC_FONTS]
    header += [f'LV_FONT_DECLARE({name})' for name, _ in MONO_FONTS]
    open(os.path.join(font_dir, 'arvel_fonts.h'), 'w').write('\n'.join(header) + '\n')


# ----------------------------------------------------------------------- main
def main():
    design = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'Arvel Buddy — ESP32 Display.html')
    screen, scene_lib, tool_icon, weather_icon, font = load_design(design)
    renderer = Renderer()

    scene_svgs = {}
    for key, svg in re.findall(r'<sc-if value="\{\{is(\w+)\}\}"[^>]*>\s*(<svg.*?</svg>)\s*</sc-if>', scene_lib, re.S):
        svg = re.sub(r'<svg[^>]*>', f'<svg xmlns="{SVG_NS}" viewBox="0 0 640 172">', svg, count=1)
        scene_svgs[key.lower()] = svg

    scenes = []
    for device_id, key, name, dark in SCENES:
        svg = scene_svgs[key]
        thumb_inner = ''.join(tostr(strip_anims(e)) for e in ET.fromstring(svg))
        thumb_svg = (f'<div style="width:80px;height:56px;border-radius:10px;overflow:hidden">'
                     f'{svg_wrap(thumb_inner, "0 0 640 172", 80, 56, "xMaxYMid slice")}</div>')
        scenes.append({'id': device_id, 'name': name, 'dark': dark,
                       'layers': scene_layers(svg, renderer), 'thumb_job': renderer.add(thumb_svg, 80, 56)})

    bird = re.search(r'<svg width="170" height="170".*?</svg>', screen, re.S).group(0)
    bird_root = ET.fromstring(re.sub(r'<svg[^>]*>', f'<svg xmlns="{SVG_NS}" width="170" height="170" viewBox="0 0 80 80">', bird, count=1))
    group = bird_root.find(f'{{{SVG_NS}}}g')
    blink_parts = [e for e in group if anims_of(e) and e.tag.split('}')[-1] in ('circle', 'ellipse')]
    for e in blink_parts:
        group.remove(e)
    strip_anims(bird_root)
    character_job = renderer.add(tostr(bird_root), 170, 170)
    for e in blink_parts:
        strip_anims(e).set('opacity', '1')
    blink_svg = svg_wrap(''.join(tostr(e) for e in blink_parts), '0 0 80 80', 170, 170)
    blink_job = renderer.add(blink_svg, 170, 170)

    icon_jobs = []
    for name, size, stroke, body in ICONS:
        svg = (f'<svg xmlns="{SVG_NS}" width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" stroke="#000" '
               f'stroke-width="{stroke}" stroke-linecap="round" stroke-linejoin="round">{body}</svg>')
        icon_jobs.append((name, renderer.add(svg, size, size)))

    # Tool icons: each kind's <g> from the ToolIcon component, without its
    # cursor blink (the device draws the icon static).
    tool_groups = dict(re.findall(r'<sc-if value="\{\{is(\w+)\}\}"[^>]*>\s*(<g>.*?</g>)\s*</sc-if>', tool_icon, re.S))
    tool_jobs = []
    for kind in TOOL_KINDS:
        group = strip_anims(ET.fromstring(tool_groups[kind.capitalize()].replace('<g>', f'<g xmlns="{SVG_NS}">', 1)))
        tool_jobs.append((kind, [renderer.add(svg_wrap(tostr(group), '0 0 32 32', size, size), size, size)
                                 for size in TOOL_SIZES]))
    # Weather icons, likewise drawn static (no sun spin, cloud drift or rain blink).
    weather_groups = dict(re.findall(r'<sc-if value="\{\{is(\w+)\}\}"[^>]*>\s*(<g>.*?</g>)\s*</sc-if>', weather_icon, re.S))
    weather_jobs = []
    for kind in WEATHER_KINDS:
        group = strip_anims(ET.fromstring(weather_groups[kind.capitalize()].replace('<g>', f'<g xmlns="{SVG_NS}">', 1)))
        weather_jobs.append((kind, [renderer.add(svg_wrap(tostr(group), '0 0 32 32', size, size), size, size)
                                    for size in WEATHER_SIZES]))

    images = renderer.run()

    for scene in scenes:
        for index, layer in enumerate(scene['layers']):
            img = images[layer['job']]
            if layer['kind'] == 'base':
                layer.update(png=png_bytes(img, 'RGB'), x=0, y=0, w=640, h=172, tracks=[])
                continue
            sprite, x, y = crop_sprite(img)
            if sprite is None:
                sys.exit(f'Empty layer {index} in scene {scene["id"]}')
            layer.update(png=png_bytes(sprite, 'RGBA'), x=x, y=y, w=sprite.width, h=sprite.height)
    thumbs = [png_bytes(images[s['thumb_job']], 'RGBA') for s in scenes]
    character = png_bytes(images[character_job], 'RGBA')
    blink_full = images[blink_job]
    box = blink_full.getchannel('A').getbbox()
    blink = {'png': png_bytes(blink_full.crop(box), 'RGBA'), 'x': box[0], 'y': box[1],
             'w': box[2] - box[0], 'h': box[3] - box[1]}
    icons = [(name, images[job]) for name, job in icon_jobs]
    tools = [(kind, [png_bytes(images[job], 'RGBA') for job in jobs]) for kind, jobs in tool_jobs]
    weather = [(kind, [png_bytes(images[job], 'RGBA') for job in jobs]) for kind, jobs in weather_jobs]

    emit_art(scenes, thumbs, character, blink, icons, tools, weather)
    build_fonts(font)
    total = sum(len(l['png']) for s in scenes for l in s['layers']) + sum(len(t) for t in thumbs)
    fonts = len(FONTS) + len(SUBSET_FONTS) + len(ITALIC_FONTS) + len(MONO_FONTS)
    print(f'Wrote arvel_art.c ({total // 1024} KiB of scene PNG) and {fonts} fonts')


if __name__ == '__main__':
    main()
