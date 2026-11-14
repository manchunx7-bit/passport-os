import os
import re

def extract_glyphs(filepath):
    if not os.path.exists(filepath):
        return set()
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    matches = re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})', content)
    return {chr(int(m, 16)) for m in matches}

fonts = {
    'buddy_font_16': extract_glyphs('main/fonts/buddy_font_16.c'),
    'ui_font_20': extract_glyphs('main/fonts/ui_font_20.c'),
}

# Scan every single file in main/
cjk_re = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]')

out_lines = []

for root, dirs, files in os.walk('main'):
    if 'fonts' in root: continue
    for file in files:
        if file.endswith(('.c', '.cc', '.cpp', '.h')):
            p = os.path.join(root, file).replace('\\', '/')
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
            
            # Find all string literals in the file
            literals = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', content)
            for s in literals:
                # ignore format specifiers only
                cjk_chars = cjk_re.findall(s)
                if not cjk_chars:
                    continue
                for c in set(cjk_chars):
                    # Check which fonts have it
                    present_in = [name for name, glyphs in fonts.items() if c in glyphs]
                    if not present_in:
                        out_lines.append(f"MISSING IN ALL FONTS: '{c}' (U+{ord(c):04X}) in string \"{s}\" at {p}")
                    elif 'buddy_font_16' not in present_in:
                        out_lines.append(f"MISSING IN buddy_font_16: '{c}' (U+{ord(c):04X}) in string \"{s}\" at {p} (present in: {present_in})")

# Remove duplicates while preserving order
seen = set()
unique_lines = []
for line in out_lines:
    if line not in seen:
        seen.add(line)
        unique_lines.append(line)

with open('tests/all_missing_audit.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(unique_lines) + '\n')

print(f"Audit completed. Found {len(unique_lines)} issues.")
