"""Every registered launcher title must be native 20px, never 16px fallback."""
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
print(f'Launcher: all {len(registered)} registered titles have native 20px glyph coverage: PASS')
