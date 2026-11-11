#!/usr/bin/env python3
"""Keeps the English catalog in step with the source.

Interface text is written in Chinese inside T("..."), Tn("...", count) or
Tc("context", "...") in src/. src/assets/i18n-en.json maps each of those texts to
English; a text with a context is stored under "@context@text". This script fails when

  * a text in the source has no English entry, or an empty one;
  * an entry is no longer used by the source;
  * the English text does not carry the same %1, %2 ... placeholders or the same
    HTML tags as the Chinese text, still contains Chinese characters, or lost the
    space or line break the Chinese text has at an end;
  * a Tn() text gives two forms ("one|many") that differ in their placeholders, or
    is also asked for with T(), which would show both forms;
  * a string literal with Chinese characters in src/ is not inside T() or Tn().
    A line that must keep such a literal ends with a comment containing "i18n: keep".

Usage: check-i18n.py            check, exit 1 on any problem
       check-i18n.py --missing  print the texts that still need an entry, as JSON
       check-i18n.py --texts    print every text in the source, as JSON
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'src' / 'assets' / 'i18n-en.json'
CJK = re.compile(r'[㐀-鿿　-〿＀-￯]')
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
CALL = re.compile(r'\b(T|Tn)\(\s*(' + LITERAL + r'(?:\s*' + LITERAL + r')*)')
CONTEXT_CALL = re.compile(r'\bTc\(\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r'(?:\s*' + LITERAL + r')*)')
ESCAPES = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", '0': '\0', 'r': '\r'}


def unescape(literal):
    """The text of one or more adjacent C string literals."""
    out = []
    for part in re.findall(LITERAL, literal):
        body, i = part[1:-1], 0
        while i < len(body):
            if body[i] == '\\':
                i += 1
                if body[i] not in ESCAPES:
                    raise SystemExit(f'unsupported escape \\{body[i]} in {part}')
                out.append(ESCAPES[body[i]])
            else:
                out.append(body[i])
            i += 1
    return ''.join(out)


def without_comments(text):
    """Source with // and /* */ comments blanked, string literals kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append('//' + ('i18n: keep' if 'i18n: keep' in text[i:j] else '')); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j])); i = j
        elif text.startswith('R"', i) and not (i and (text[i - 1].isalnum() or text[i - 1] == '_')):
            # R"delimiter( ... )delimiter": kept out of the scan, with its line breaks.
            opening = text.index('(', i)
            closing = ')' + text[i + 2:opening] + '"'
            j = text.index(closing, opening) + len(closing)
            if CJK.search(text[i:j]):
                raise SystemExit('a raw string literal holds Chinese text; write it as an ordinary literal inside T()')
            out.append('""' + '\n' * text.count('\n', i, j)); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def source_texts():
    """{text: {'plural': bool, 'where': [file:line, ...]}} and the unwrapped literals."""
    texts, unwrapped = {}, []
    for path in sorted((ROOT / 'src').glob('*.cpp')) + sorted((ROOT / 'src').glob('*.h')):
        if path.name in ('i18n.cpp', 'i18n.h'):
            continue
        code = without_comments(path.read_text(encoding='utf-8'))
        wrapped = []
        for match in CALL.finditer(code):
            text = unescape(match.group(2))
            line = code.count('\n', 0, match.start()) + 1
            entry = texts.setdefault(text, {'plural': False, 'plain': False, 'where': []})
            entry['plural'] |= match.group(1) == 'Tn'
            entry['plain'] |= match.group(1) == 'T'
            entry['where'].append(f'{path.name}:{line}')
            wrapped.append((match.start(2), match.end(2)))
        for match in CONTEXT_CALL.finditer(code):
            key = '@' + unescape(match.group(1)) + '@' + unescape(match.group(2))
            texts.setdefault(key, {'plural': False, 'where': []})['where'].append(f'{path.name}:{code.count(chr(10), 0, match.start()) + 1}')
            wrapped.append((match.start(1), match.end(2)))
        for match in re.finditer(LITERAL, code):
            if not CJK.search(match.group(0)) or any(a <= match.start() < b for a, b in wrapped):
                continue
            end = code.find('\n', match.end())
            if 'i18n: keep' in code[match.end():end if end >= 0 else len(code)]:
                continue
            unwrapped.append(f'{path.name}:{code.count(chr(10), 0, match.start()) + 1}: {match.group(0)[:70]}')
    return texts, unwrapped


def placeholders(text):
    return sorted(set(re.findall(r'%\d+', text)))


def tags(text):
    return sorted(re.findall(r'</?[a-zA-Z][a-zA-Z0-9]*', text))


def main():
    texts, unwrapped = source_texts()
    catalog = json.loads(CATALOG.read_text(encoding='utf-8'))
    if '--texts' in sys.argv:
        print(json.dumps(sorted(texts), ensure_ascii=False, indent=0)); return 0
    missing = [text for text in texts if not catalog.get(text)]
    if '--missing' in sys.argv:
        print(json.dumps({text: '' for text in sorted(missing)}, ensure_ascii=False, indent=0)); return 0
    problems = [f'no English for {text!r} ({texts[text]["where"][0]})' for text in missing]
    problems += [f'entry no longer used: {text!r}' for text in catalog if text not in texts]
    problems += [f'Chinese literal outside T(): {where}' for where in unwrapped]
    problems += [f'used with Tn() and with T(); a text about a count always needs its count: {text!r} ({", ".join(entry["where"])})'
                 for text, entry in texts.items() if entry['plural'] and entry.get('plain')]
    for key, english in catalog.items():
        if key not in texts or not english:
            continue
        # "@context@text" is compared by its text.
        text = key.split('@', 2)[2] if key.startswith('@') else key
        forms = english.split('|') if texts[key]['plural'] else [english]
        if len(forms) > 2:
            problems.append(f'more than two forms for {text!r}')
        for form in forms:
            if placeholders(form) != placeholders(text):
                problems.append(f'placeholders differ: {text!r} -> {form!r}')
            if tags(form) != tags(text):
                problems.append(f'HTML tags differ: {text!r} -> {form!r}')
            if CJK.search(form):
                problems.append(f'Chinese left in the English text: {text!r} -> {form!r}')
            # Space at either end joins the text to its neighbours. English may add some
            # (", " for "，") but must not lose any, and a final line break must stay.
            if (text[:1].isspace() and not form[:1].isspace()) or (text[-1:].isspace() and not form[-1:].isspace()) or text.endswith('\n') != form.endswith('\n'):
                problems.append(f'space or line break at an end was lost: {text!r} -> {form!r}')
    for problem in problems:
        print(problem)
    print(json.dumps({'result': 'FAIL' if problems else 'PASS', 'texts': len(texts), 'entries': len(catalog), 'problems': len(problems)}, ensure_ascii=False))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
