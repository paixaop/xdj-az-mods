#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Conservative whole-function matching for extracted C coverage fixtures.

Only complete, uniquely matching definitions count. Indentation may differ;
internal whitespace, strings, directives, signatures and every body line must
match. Unrecognized C constructs remain unmeasured instead of borrowing hits
from a nearby matching fragment. Compatible with the test image's Python 3.6.
"""
import re
from collections import defaultdict


def syntax_text(text):
    """Mask comments, literals and directives without moving source offsets."""
    pattern = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|^[ \t]*\#(?:[^\n]*\\\n)*[^\n]*',
                         re.DOTALL | re.MULTILINE)
    return pattern.sub(lambda match: ''.join('\n' if c == '\n' else ' ' for c in match.group()), text)


def definitions(text):
    """Yield (name, first line, last line, stripped lines) for C definitions."""
    masked = syntax_text(text)
    tokens = list(re.finditer(r'[A-Za-z_][A-Za-z_0-9]*|[^\s]', masked))
    lines = text.splitlines()
    depth = parens = 0
    start = 0
    name = None
    body = None
    assignment = False
    ignored = {'__attribute__', '__attribute', '__declspec', 'asm', '__asm__',
               'typeof', '__typeof__', 'if', 'for', 'while', 'switch'}
    for i, token in enumerate(tokens):
        value = token.group()
        if depth == 0:
            if value == '(':
                if parens == 0 and i > start:
                    previous = tokens[i - 1].group()
                    if re.match(r'^[A-Za-z_][A-Za-z_0-9]*$', previous) and previous not in ignored:
                        name = previous
                parens += 1
            elif value == ')':
                parens = max(0, parens - 1)
            elif value == '=':
                assignment = True
            elif value == ';':
                start, name, assignment = i + 1, None, False
            elif value == '{':
                depth = 1
                if name is not None and not assignment and parens == 0 and i > start:
                    body = (name, text.count('\n', 0, tokens[start].start()) + 1)
        elif value == '{':
            depth += 1
        elif value == '}':
            depth -= 1
            if depth == 0:
                if body is not None:
                    symbol, first = body
                    last = text.count('\n', 0, token.end()) + 1
                    yield symbol, first, last, tuple(line.strip() for line in lines[first - 1:last])
                start, name, body, assignment = i + 1, None, None, False


class FunctionIndex:
    def __init__(self, sources):
        self.matches = defaultdict(list)
        for path, text in sources.items():
            for name, first, last, body in definitions(text):
                self.matches[(name, body)].append((path, first, last))

    def align(self, text):
        mapping = {}
        for name, first, last, body in definitions(text):
            candidates = self.matches.get((name, body), ())
            if len(candidates) != 1:
                continue
            path, original, _ = candidates[0]
            for line in range(first, last + 1):
                mapping[line] = (path, original + line - first)
        return mapping
