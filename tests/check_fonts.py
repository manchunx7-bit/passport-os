import os
import re

def extract_glyphs_from_c_font(filepath):
    if not os.path.exists(filepath):
        return set()
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    matches = re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})', content)
    return {chr(int(m, 16)) for m in matches}

fonts = {
    'buddy_font_16': extract_glyphs_from_c_font('main/fonts/buddy_font_16.c'),
    'ui_font_20': extract_glyphs_from_c_font('main/fonts/ui_font_20.c'),
}

cjk_re = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]')

char_usage = {}
for root, dirs, files in os.walk('main'):
    if 'fonts' in root:
        continue
    for file in files:
        if file.endswith(('.c', '.cc', '.cpp', '.h')):
            p = os.path.join(root, file)
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                for line_idx, line in enumerate(f, 1):
                    # extract string literals: "..."
                    strs = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', line)
                    for s in strs:
                        chars = set(cjk_re.findall(s))
                        for c in chars:
                            if c not in char_usage:
                                char_usage[c] = []
                            char_usage[c].append((p.replace('\\', '/'), line_idx, s))

lines = []
lines.append(f"Total unique CJK chars in main/ string literals: {len(char_usage)}")
for font_name, glyphs in fonts.items():
    lines.append(f"Font {font_name}: {len(glyphs)} glyphs")

missing_in_buddy = {c: occ for c, occ in char_usage.items() if c not in fonts['buddy_font_16']}
lines.append(f"\nMissing in buddy_font_16: {len(missing_in_buddy)} characters:")

for c in sorted(missing_in_buddy.keys()):
    occ = missing_in_buddy[c]
    first_occ = occ[0]
    in_other = [fn for fn, gl in fonts.items() if c in gl]
    lines.append(f"  '{c}' (U+{ord(c):04X}): used in [{first_occ[2]}] at {first_occ[0]}:{first_occ[1]}, in other fonts: {in_other}")

with open('tests/font_report.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')

print("Report written to tests/font_report.txt successfully")
