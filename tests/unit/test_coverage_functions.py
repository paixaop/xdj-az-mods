#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reject altered fixtures even when most production lines still match."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), str(HERE / (name + '.py')))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FUNCTION = '''static int select_value(int value)
{
    if (value > 7) return 4;
    if (value < 2) return 0;
    return value;
}
'''
OTHER = '''int other(void)
{
    return 2;
}
'''


class FunctionCoverageTests(unittest.TestCase):
    def setUp(self):
        self.tool = load('coverage-functions')
        self.source = '/* production */\n' + FUNCTION + '\n' + OTHER
        self.index = self.tool.FunctionIndex({'azmod/example.c': self.source})

    def test_complete_definition_and_indentation_preserve_original_lines(self):
        copy = '#include "fixture.h"\n\n' + '\n'.join('  ' + line for line in FUNCTION.splitlines())
        mapping = self.index.align(copy)
        self.assertEqual(mapping, {line: ('azmod/example.c', line - 1) for line in range(3, 9)})

    def test_mapped_lcov_preserves_zero_lines_without_inventing_uncompiled_functions(self):
        tool = load('coverage-report')
        # Two actual compiler reports share a line; the second adds a zero-hit line.
        # The neighboring production function was never compiled by either test.
        hits = {('azmod/example.c', 2): 1, ('azmod/example.c', 4): 0}
        second = {('azmod/example.c', 2): 0, ('azmod/example.c', 5): 0,
                  ('azstem/other.c', 7): 3}
        for key, count in second.items():
            hits[key] = max(hits.get(key, 0), count)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'coverage.lcov'
            tool.write_mapped_lcov(str(path), hits)
            report = tool.parse_lcov(path.read_text())
        self.assertEqual(set(report), {'guest/azmod/example.c', 'guest/azstem/other.c'})
        self.assertEqual(report['guest/azmod/example.c']['DA'], {2: 1, 4: 0, 5: 0})
        self.assertEqual(report['guest/azstem/other.c']['DA'], {7: 3})
        self.assertNotIn(9, report['guest/azmod/example.c']['DA'])

    def test_mutated_function_cannot_borrow_its_unchanged_lines(self):
        copy = FUNCTION.replace('value > 7', 'value > 8') + '\n' + OTHER
        mapping = self.index.align(copy)
        self.assertTrue(mapping)
        self.assertEqual(set(mapping), set(range(8, 12)))
        self.assertFalse(self.index.align(FUNCTION.replace('return value;', 'return 9;')))

    def test_signature_and_dev_path_rewrites_are_not_production(self):
        self.assertFalse(self.index.align(FUNCTION.replace('select_value', 'fixture_value')))
        source = 'int device(void)\n{\n    return open("/dev/audio", 0);\n}\n'
        index = self.tool.FunctionIndex({'azmod/device.c': source})
        self.assertFalse(index.align(source.replace('"/dev/', 'FDT_DEV "/')))

    def test_partial_body_and_truncated_definition_do_not_map(self):
        self.assertFalse(self.index.align('\n'.join(FUNCTION.splitlines()[1:])))
        self.assertFalse(self.index.align(FUNCTION.rsplit('}', 1)[0]))

    def test_literal_whitespace_and_directives_are_significant(self):
        source = 'int label(void)\n{\n#if ENABLED\n    return puts("a  b");\n#else\n    return 0;\n#endif\n}\n'
        index = self.tool.FunctionIndex({'azmod/label.c': source})
        self.assertTrue(index.align(source))
        self.assertFalse(index.align(source.replace('a  b', 'a b')))
        self.assertFalse(index.align(source.replace('#if ENABLED', '#if 1')))

    def test_comments_literals_and_global_initializers_do_not_change_brace_matching(self):
        source = '''static const int values[] = {1, 2};
#define UNUSED(x) { (x); }
static int choose(
    int index) __attribute__((unused))
{
    /* } not a closing brace */
    const char *brace = "}";
    char close = '}';
    return values[index] + brace[0] + close;
}
'''
        definitions = list(self.tool.definitions(source))
        self.assertEqual([(name, first, last) for name, first, last, _ in definitions], [('choose', 3, 10)])

    def test_ambiguous_identical_definitions_are_not_guessed(self):
        index = self.tool.FunctionIndex({'azmod/a.c': FUNCTION, 'azstem/b.c': FUNCTION})
        self.assertFalse(index.align(FUNCTION))

    def test_llvm_guest_mapper_rejects_mutation_and_retains_unchanged_neighbor(self):
        with tempfile.TemporaryDirectory() as directory:
            guest = Path(directory) / 'guest/azmod'
            guest.mkdir(parents=True)
            (guest / 'example.c').write_text(self.source)
            mapper = load('coverage-report').Guest(directory)
            self.assertEqual(mapper.align(FUNCTION), self.index.align(FUNCTION))
            self.assertEqual(mapper.align(FUNCTION.replace('value > 7', 'value > 8') + OTHER),
                             self.index.align(FUNCTION.replace('value > 7', 'value > 8') + OTHER))

    def check_gcov_mutation(self, source_path):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            guest = root / 'guest/azmod'
            guest.mkdir(parents=True)
            (guest / 'example.c').write_text(self.source)
            reports = root / 'gcov/suite/object'
            reports.mkdir(parents=True)
            fixture = FUNCTION.replace('value > 7', 'value > 8') + '\n' + OTHER
            rows = ['        -:    0:Source:' + source_path]
            rows += ['        1:%5d:%s' % (line, text) for line, text in enumerate(fixture.splitlines(), 1)]
            (reports / 'fixture.gcov').write_text('\n'.join(rows) + '\n')
            out = root / 'report'
            result = subprocess.run([sys.executable, str(HERE / 'coverage-gcc-map.py'),
                                     str(root / 'guest'), str(root / 'gcov'), str(out)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            lcov = (out / 'coverage.info').read_text()
            self.assertIn('DA:9,1', lcov)
            self.assertNotIn('DA:3,', lcov)
            self.assertNotIn('DA:5,', lcov)
            self.assertIn(source_path, (out / 'unmapped.txt').read_text())

    def test_gcov_pipeline_does_not_credit_mutated_fixture(self):
        self.check_gcov_mutation('/tmp/extracted.inc')

    def test_gcov_production_path_does_not_bypass_source_validation(self):
        self.check_gcov_mutation('/g/azmod/example.c')


if __name__ == '__main__':
    unittest.main()
