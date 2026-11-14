"""Every fixed string shown in the 20px title font must have native 20px glyphs.

ui_font_20 falls back to the 16px body font, so a missing glyph does not fail
to render: it silently mixes two sizes in one title. This finds each label
created with a title font, then collects the literals it can show: literals in
the creating call, in later set_text calls on that label, in variables passed
to set_text, and in helper functions called there. Member fields (text sent
from a computer or phone) cannot be checked statically and are listed.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TITLE_FONTS = {
    'ui_font_20': ['main/fonts/ui_font_20.c'],
    'pomodoro_font_20': ['main/apps/pomodoro/pomodoro_font_20.c', 'main/fonts/ui_font_20.c'],
}
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def glyphs(paths):
    found = set()
    for p in paths:
        found |= {chr(int(m, 16)) for m in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})', (ROOT / p).read_text(encoding='utf8'))}
    return found


def statements(text):
    """Yield (start, statement) for each ';'-terminated statement, ignoring // comments."""
    clean = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group()), text)
    start = 0
    for i, ch in enumerate(clean):
        if ch in ';{}':
            yield start, clean[start:i]
            start = i + 1


def literals(expr):
    return [re.sub(r'\\.', '', s) for s in LITERAL.findall(expr)]


def function_literals(text, name):
    match = re.search(r'\b' + name + r'\s*\([^;{)]*\)\s*\{', text)
    if not match:
        return []
    depth, i = 1, match.end()
    while depth and i < len(text):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return literals(text[match.end():i])


def member_values(expr):
    """Member fields used as a displayed value, not as a condition or argument."""
    bare = LITERAL.sub('""', expr)
    return [m.group(1) for m in re.finditer(r'(\w+(?:->|\.)\w+)(\[[^]]*\])?\s*(.?)', bare)
            if not m.group(2) and m.group(3) in (':', ';', '', ')') and
            not re.search(r'\w\s*\($', bare[:m.start()]) and not bare[m.end() - 1:].startswith(')')]


def shown_strings(text, statement):
    """Literals a title label can display, plus any unchecked dynamic sources."""
    found, dynamic = literals(statement), []
    target = re.match(r'\s*(?:auto\s*\*\s*|lv_obj_t\s*\*\s*)?(\w+)\s*=', statement)
    if not target:
        return found, dynamic
    label = target.group(1)
    for _, other in statements(text):
        call = re.search(r'set_text\s*\(\s*' + label + r'\s*,(.*)', other, re.S)
        if not call:
            continue
        expr = call.group(1)
        found += literals(expr)
        bare = LITERAL.sub('""', expr)
        dynamic += member_values(expr)
        for fn in re.findall(r'\b(\w+)\s*\(', bare):
            found += function_literals(text, fn)
        for ident in set(re.findall(r'\b([A-Za-z_]\w*)\b', re.sub(r'\w+(?:->|\.)\w+|\b\w+\s*\(', '', bare))):
            for _, assign in statements(text):
                rhs = re.search(r'(?:^|[\s,*])' + ident + r'\s*=(?!=)(.*)', assign, re.S)
                if rhs:
                    found += literals(rhs.group(1))
                    dynamic += member_values(rhs.group(1))
    return found, dynamic


def main():
    failures, checked, unchecked = [], 0, []
    for path in sorted((ROOT / 'main').rglob('*')):
        if path.suffix not in ('.c', '.cc') or 'fonts' in path.parts or path.name.endswith('_font_20.c'):
            continue
        text = path.read_text(encoding='utf8')
        for font, sources in TITLE_FONTS.items():
            if '&' + font not in text:
                continue
            available = glyphs(sources)
            for _, statement in statements(text):
                if '&' + font not in statement or 'set_style_text_font' in statement:
                    continue
                strings, dynamic = shown_strings(text, statement)
                rel = path.relative_to(ROOT).as_posix()
                unchecked += [f'{rel}: {d}' for d in dynamic]
                for s in strings:
                    checked += 1
                    missing = sorted({c for c in s if c not in available and c != '\n'})
                    if missing:
                        failures.append(f'{rel}: "{s}" missing {"".join(missing)} in {font}')
    for f in failures:
        print('FAIL', f)
    for u in sorted(set(unchecked)):
        print('NOTE dynamic title text, not checked:', u)
    if failures:
        sys.exit(1)
    print(f'Title glyphs: {checked} fixed title strings have native 20px glyphs: PASS')


if __name__ == '__main__':
    main()
