"""Every registered launcher app must have native 20px title glyphs and a unique 17x17 icon."""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
registry=(root/'main/launcher/app_registry.cc').read_text(encoding='utf-8')
array=registry.split('const passport_app_t *s_apps[] = {',1)[1].split('};',1)[0]
registered=re.findall(r'&g_(\w+)_app',array)
glyphs={chr(int(c,16)) for c in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})',(root/'main/fonts/ui_font_20.c').read_text(encoding='utf-8'))}
apps={}
for path in (root/'main').rglob('*.cc'):
    source=path.read_text(encoding='utf-8')
    for match in re.finditer(r'const passport_app_t g_(\w+)_app\s*=\s*\{([\s\S]+?)\};',source):
        name=re.search(r'\.name\s*=\s*"([^"]+)"',match[2])
        if name:apps[match[1]]=name[1]
for key in registered:
    assert key in apps,key+' has no app title'
    missing=set(apps[key])-glyphs
    assert not missing,(apps[key],'missing 20px glyphs',''.join(sorted(missing)))

launcher=(root/'main/launcher/launcher.cc').read_text(encoding='utf-8')
glyph_block=launcher.split('kIconGlyphs[] = {',1)[1].split('};',1)[0]
icons={}
for match in re.finditer(r'\{(APP_ID_\w+)\s*,\s*\{([\s\S]*?)\}\}',glyph_block):
    rows=tuple(re.findall(r'"([^"]*)"',match[2]))
    assert len(rows)==17,(match[1],len(rows))
    assert all(len(r)==17 and set(r)<={'.','#'} for r in rows),(match[1],'bad row shape/chars')
    assert any('#' in r for r in rows),(match[1],'empty icon')
    icons[match[1]]=rows
for key in registered:
    app_id=f'APP_ID_{key.upper()}'
    assert app_id in icons,f'{app_id} missing from kIconGlyphs'
assert len(set(icons.values()))==len(icons),'duplicate launcher icons in kIconGlyphs'

palette_h=(root/'main/ui/ui_palette.h').read_text(encoding='utf-8')
assert 'UI_THEME_STUDIO' in palette_h,'UI_THEME_STUDIO missing from ui_palette.h'
pixel_c=(root/'main/ui/ui_pixel.c').read_text(encoding='utf-8')
portrait_c=(root/'main/ui/ui_portrait.c').read_text(encoding='utf-8')
theme_names=re.findall(r'case UI_THEME_(\w+):\s*return\s*"([^"]+)"',portrait_c)
assert any(k=='STUDIO' and v=='曜石工坊' for k,v in theme_names),'Studio theme name missing'
buddy_glyphs={chr(int(c,16)) for c in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})',(root/'main/fonts/buddy_font_16.c').read_text(encoding='utf-8'))}
for k,v in theme_names:
    missing_16=set(v)-buddy_glyphs
    assert not missing_16,(k,v,'missing 16px glyphs',''.join(sorted(missing_16)))
theme_enum=re.findall(r'UI_THEME_(\w+)\s*=\s*(\d+)',palette_h)
for name,_ in theme_enum:
    assert f'[UI_THEME_{name}]' in pixel_c,f'UI_THEME_{name} palette missing'

print(f'Launcher: all {len(registered)} registered titles have native 20px glyphs and unique 17x17 icons ({len(theme_enum)} themes verified): PASS')
