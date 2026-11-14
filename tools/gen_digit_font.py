"""Extract a glyph subset from one of LVGL's built-in Montserrat fonts.

Copies bitmaps, metrics and kerning byte-for-byte, so the subset renders
exactly like the full font while linking only the characters a readout uses.
Rasterizing again with lv_font_conv differs from the shipped bitmaps by a few
grey levels, which is why this copies instead of regenerating.

    python tools/gen_digit_font.py 32 " .0123456789:ENT" ui_digits_32
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def array(text, name):
    body = re.search(r'\b' + name + r'\[\]\s*=\s*\{(.*?)\};', text, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    return [int(v, 0) for v in re.findall(r'-?0x[0-9a-fA-F]+|-?\d+', body)]


def field(text, name):
    return int(re.search(r'\.' + name + r'\s*=\s*(-?\d+)', text).group(1))


def rows(values, per_line=16, fmt=str):
    return ',\n'.join('    ' + ', '.join(fmt(v) for v in values[i:i + per_line])
                      for i in range(0, len(values), per_line))


def main(size, chars, name):
    source = ROOT / f'managed_components/lvgl__lvgl/src/font/lv_font_montserrat_{size}.c'
    text = source.read_text(encoding='utf8')
    bitmap = array(text, 'glyph_bitmap')
    dsc = re.search(r'glyph_dsc\[\]\s*=\s*\{(.*?)\};', text, re.S).group(1)
    glyphs = [dict((k, int(v)) for k, v in re.findall(r'\.(\w+)\s*=\s*(-?\d+)', g))
              for g in re.findall(r'\{([^{}]*)\}', dsc)]
    left, right = array(text, 'kern_left_class_mapping'), array(text, 'kern_right_class_mapping')
    values, right_count = array(text, 'kern_class_values'), field(text, 'right_class_cnt')
    bpp = field(text, 'bpp')

    cps = sorted(set(map(ord, chars)))
    assert all(0x20 <= cp < 0x7f for cp in cps), 'only the contiguous ASCII range is supported'
    out_bitmap, out_dsc = [], [dict(bitmap_index=0, adv_w=0, box_w=0, box_h=0, ofs_x=0, ofs_y=0)]
    for cp in cps:
        g = glyphs[cp - 0x20 + 1]
        size_bytes = (g['box_w'] * g['box_h'] * bpp + 7) // 8
        out_dsc.append(dict(g, bitmap_index=len(out_bitmap)))
        out_bitmap += bitmap[g['bitmap_index']:g['bitmap_index'] + size_bytes]

    gids = [cp - 0x20 + 1 for cp in cps]
    used_left = sorted({left[g] for g in gids} - {0})
    used_right = sorted({right[g] for g in gids} - {0})
    new_left = [0] + [used_left.index(left[g]) + 1 if left[g] else 0 for g in gids]
    new_right = [0] + [used_right.index(right[g]) + 1 if right[g] else 0 for g in gids]
    new_values = [values[(l - 1) * right_count + (r - 1)] for l in used_left for r in used_right]

    dsc_lines = ',\n'.join(
        '    {.bitmap_index = %(bitmap_index)d, .adv_w = %(adv_w)d, .box_w = %(box_w)d, .box_h = %(box_h)d, '
        '.ofs_x = %(ofs_x)d, .ofs_y = %(ofs_y)d}' % g for g in out_dsc)
    metrics = {k: field(text, k) for k in ('line_height', 'base_line', 'underline_position', 'underline_thickness')}
    output = f'''/*******************************************************************************
 * Size: {size} px
 * Bpp: {bpp}
 * Subset of LVGL's built-in lv_font_montserrat_{size}, copied byte-for-byte.
 * Glyphs: "{chars}"
 * Regenerate: python tools/gen_digit_font.py {size} "{chars}" {name}
 ******************************************************************************/

#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{rows(out_bitmap, 16, lambda v: f'0x{v:02x}')}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{dsc_lines}
}};

static const uint16_t unicode_list_0[] = {{
{rows([cp - cps[0] for cp in cps], 16, lambda v: f'0x{v:x}')}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{
        .range_start = {cps[0]}, .range_length = {cps[-1] - cps[0] + 1}, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = {len(cps)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }}
}};

static const uint8_t kern_left_class_mapping[] = {{
{rows(new_left)}
}};

static const uint8_t kern_right_class_mapping[] = {{
{rows(new_right)}
}};

static const int8_t kern_class_values[] = {{
{rows(new_values)}
}};

static const lv_font_fmt_txt_kern_classes_t kern_classes = {{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = {len(used_left)},
    .right_class_cnt     = {len(used_right)},
}};

static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = {field(text, 'kern_scale')},
    .cmap_num = 1,
    .bpp = {bpp},
    .kern_classes = 1,
    .bitmap_format = 0,
}};

const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {metrics['line_height']},
    .base_line = {metrics['base_line']},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = {metrics['underline_position']},
    .underline_thickness = {metrics['underline_thickness']},
    .dsc = &font_dsc,
}};
'''
    (ROOT / 'main/fonts' / f'{name}.c').write_text(output, encoding='utf8', newline='\n')
    print(f'{name}: {len(cps)} glyphs, {len(out_bitmap)} bitmap bytes')


if __name__ == '__main__':
    main(int(sys.argv[1]), sys.argv[2], sys.argv[3])
