"""The 32px digit subset must render exactly like LVGL's built-in Montserrat 32.

Compares, for every glyph in the subset: bitmap bytes, advance, box and offsets;
kerning for every glyph pair; and the line metrics that position labels.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FULL = ROOT / 'managed_components/lvgl__lvgl/src/font/lv_font_montserrat_32.c'
SUBSET = ROOT / 'main/fonts/ui_digits_32.c'
# Every character the 32px readouts can show: scores, frequencies, timers,
# the setup address and the radio's "NET 01" fallback.
REQUIRED = ' .0123456789:ENT'


def array(text, name):
    body = re.search(r'\b' + name + r'\[\]\s*=\s*\{(.*?)\};', text, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    return [int(v, 0) for v in re.findall(r'-?0x[0-9a-fA-F]+|-?\d+', body)]


def field(text, name):
    return int(re.search(r'\.' + name + r'\s*=\s*(-?\d+)', text).group(1))


class Font:
    def __init__(self, path, codepoints):
        text = path.read_text(encoding='utf8')
        self.bitmap = array(text, 'glyph_bitmap')
        dsc = re.search(r'glyph_dsc\[\]\s*=\s*\{(.*?)\};', text, re.S).group(1)
        self.glyphs = [dict((k, int(v)) for k, v in re.findall(r'\.(\w+)\s*=\s*(-?\d+)', g))
                       for g in re.findall(r'\{([^{}]*)\}', dsc)]
        self.left = array(text, 'kern_left_class_mapping')
        self.right = array(text, 'kern_right_class_mapping')
        self.values = array(text, 'kern_class_values')
        self.right_count = field(text, 'right_class_cnt')
        self.scale = field(text, 'kern_scale')
        self.metrics = {k: field(text, k) for k in ('line_height', 'base_line', 'underline_position', 'underline_thickness')}
        self.gid = codepoints

    def glyph(self, cp):
        g = self.glyphs[self.gid[cp]]
        size = (g['box_w'] * g['box_h'] * 4 + 7) // 8
        return {k: g[k] for k in ('adv_w', 'box_w', 'box_h', 'ofs_x', 'ofs_y')}, self.bitmap[g['bitmap_index']:g['bitmap_index'] + size]

    def kern(self, a, b):
        l, r = self.left[self.gid[a]], self.right[self.gid[b]]
        return self.values[(l - 1) * self.right_count + (r - 1)] * self.scale if l and r else 0


subset_text = SUBSET.read_text(encoding='utf8')
start = int(re.search(r'\.range_start\s*=\s*(\d+)', subset_text).group(1))
offsets = array(subset_text, 'unicode_list_0')
subset_cps = [start + o for o in offsets]
assert sorted(map(ord, REQUIRED)) == subset_cps, 'subset glyphs differ from the readout repertoire'

subset = Font(SUBSET, {cp: i + 1 for i, cp in enumerate(subset_cps)})
# Labels are positioned with the full font's metrics; these must never drift.
assert subset.metrics == {'line_height': 35, 'base_line': 6, 'underline_position': -2, 'underline_thickness': 2}, subset.metrics
if not FULL.exists():
    # managed_components/ is fetched by the ESP-IDF build, not tracked in git.
    print('Digit font: repertoire and line metrics PASS; byte comparison SKIPPED (run an ESP-IDF build to fetch LVGL)')
    raise SystemExit(0)
full = Font(FULL, {cp: cp - 0x20 + 1 for cp in range(0x20, 0x7f)})
assert full.metrics == subset.metrics, (full.metrics, subset.metrics)
for cp in subset_cps:
    assert full.glyph(cp) == subset.glyph(cp), f'glyph U+{cp:04X} differs'
for a in subset_cps:
    for b in subset_cps:
        assert full.kern(a, b) == subset.kern(a, b), f'kerning {chr(a)!r}{chr(b)!r} differs'
print(f'Digit font: {len(subset_cps)} glyphs, {len(subset_cps) ** 2} kerning pairs and line metrics match Montserrat 32: PASS')
