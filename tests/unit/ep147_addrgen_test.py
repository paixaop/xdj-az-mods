#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The local generator must publish the reviewed native cache address pairs."""
from pathlib import Path
import json
import copy
import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import tempfile
import re
import unittest
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('ep147_addrgen', ROOT / 'tools/ep147-addrgen.py')
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
def table(text, version):
    enum = text[text.index('enum {'):text.index('EPI_N')]
    names = re.findall(r'EPI_([0-9a-f]+)', enum)
    array = text[text.index('static const uint32_t ep147_addr_v' + version):]
    array = array[:array.index('};')]
    values = re.findall(r'0x([0-9a-f]+),', array)
    return {'EP_' + n: int(v, 16) for n, v in zip(names, values)}
class AddressGeneratorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.firmware = Path(os.environ.get('EP147_FIRMWARE_DIR', ROOT.parent / 'anchor/firmware'))
        if not all((cls.firmware / ('EP147-' + v)).is_file() for v in GEN.VERSIONS):
            raise unittest.SkipTest('supply both stock ELFs with EP147_FIRMWARE_DIR')
        cls.config = GEN.read_catalog(ROOT / 'tools/ep147-addresses.json')
        cls.images = GEN.load_images(cls.config, cls.firmware)
    def changed(self):
        return copy.deepcopy(self.config)
    def test_slip_owner_lifecycle_targets_are_generated(self):
        self.assertTrue('slip_observer' in self.config,'missing reviewed Slip construction/publication/disposal bindings')
        with tempfile.TemporaryDirectory() as folder:
            root=self.fixture(folder);outputs=GEN.generate(root,self.config,self.images)
            self.assertIn(root/'src/azmod/sdk_slip_targets.h',outputs)
            for version in GEN.VERSIONS:
                text=outputs[root/'src/azmod/sdk_slip_targets.h']
                for entry in self.config['slip_observer']['entries']:
                    self.assertIn(entry['profiles'][version]['sha256'],text)
    def test_slip_changed_signature_rejected(self):
        cfg=self.changed();cfg['slip_observer']['entries'][0]['profiles']['2.00']['sha256']='0'*64
        with self.assertRaisesRegex(GEN.MappingError,'Slip signature'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_slip_getter_sites_generated(self):
        text=GEN.render_slip_targets(self.config)
        for p in self.config['slip_observer']['profiles'].values():
            self.assertIn(p['producer']['sha256'],text)
            for site in p['getter_sites']:self.assertIn(site,text)
    def test_slip_getter_body_changed_rejected(self):
        cfg=self.changed();cfg['slip_observer']['profiles']['2.00']['producer']['sha256']='0'*64
        with self.assertRaisesRegex(GEN.MappingError,'Slip producer.*signature'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_slip_getter_noncall_rejected(self):
        cfg=self.changed();p=cfg['slip_observer']['profiles']['1.30'];p['getter_sites'][0]=hex(int(p['getter_sites'][0],16)+4)
        with self.assertRaisesRegex(GEN.MappingError,'Slip getter.*call'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_slip_wrong_owner_type_rejected(self):
        cfg=self.changed();p=cfg['slip_observer']['profiles']['1.30'];p['source_control_vtable']=p['parent_control_vtable']
        with self.assertRaisesRegex(GEN.MappingError,'Slip genuine.*control'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_slip_wrong_constructor_caller_rejected(self):
        cfg=self.changed();cfg['slip_observer']['profiles']['2.00']['parent_returns'][0]='0x2139cc8'
        with self.assertRaisesRegex(GEN.MappingError,'Slip owning call'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_reply_promotion_comparison_profile_is_generated(self):
        with tempfile.TemporaryDirectory() as folder:
            root=self.fixture(folder)
            outputs=GEN.generate(root,self.config,self.images)
            header=root/'src/common/azcachereplycode.h'
            self.assertTrue(header in outputs,'missing generated original-code comparison for native X21 promotion')
            text=outputs[header]
            for version in GEN.VERSIONS:
                self.assertIn(self.config['reply_promotion']['profiles'][version]['sha256'],text)
    def test_reply_promotion_changed_code_rejected(self):
        cfg=self.changed();cfg['reply_promotion']['profiles']['1.30']['sha256']='0'*64
        with self.assertRaisesRegex(GEN.MappingError,'promotion.*signature'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_reply_promotion_wrong_callsite_rejected(self):
        cfg=self.changed();row=cfg['reply_promotion']['profiles']['2.00']
        row['return_pc']=hex(int(row['return_pc'],16)+4)
        with self.assertRaisesRegex(GEN.MappingError,'promotion.*call'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_reply_promotion_wrong_register_rejected(self):
        cfg=self.changed();cfg['reply_promotion']['control_register']=20
        with self.assertRaisesRegex(GEN.MappingError,'promotion.*register'):
            GEN.validate(cfg,self.images,ROOT/'src')
    def test_selected_reply_and_reception_entries_have_distinct_generated_slots(self):
        for name,kind,first,second in [
                ('EP_10b29f8','AZCE_KIND_REPLY_TICKET',0x10b29f8,0x1142bc8),
                ('EP_a28550','AZCE_KIND_REPLY_RECEPTION',0xa28550,0xa6d960)]:
            self.assertTrue(name in self.config['cache_entries'],'missing actual reply handoff mapping: '+name)
            row=self.config['cache_entries'][name]
            self.assertEqual(row['kind'],kind)
            self.assertEqual([int(row['profiles'][v]['entry'],16) for v in GEN.VERSIONS],[first,second])
    def fixture(self, folder):
        root = Path(folder)
        for library, alias in [('azmod', 'aabb78'), ('azstem', '14a2560')]:
            (root / 'src' / library).mkdir(parents=True)
            (root / 'src' / library / 'use.c').write_text('long f(void) { return EP_' + alias + '; }\n')
        (root / 'src/common').mkdir()
        shutil.copyfile(ROOT / 'src/common/ep147build.h', root / 'src/common/ep147build.h')
        shutil.copyfile(ROOT / 'src/common/azcacheentry.h', root / 'src/common/azcacheentry.h')
        return root
    def cli(self, root, *extra):
        args = ['--root', str(root), '--mapping', str(ROOT / 'tools/ep147-addresses.json'),
                '--firmware-dir', str(self.firmware)] + list(extra)
        with contextlib.redirect_stderr(io.StringIO()):
            return GEN.main(args)
    def test_reviewed_cache_targets_are_generated_for_both_builds(self):
        config = json.loads((ROOT / 'tools/ep147-addresses.json').read_text())
        header = (ROOT / 'src/azmod/ep147addr.h').read_text()
        for version, suffix in [('1.30', '130'), ('2.00', '200')]:
            actual = table(header, suffix)
            expected = {name: int(row['profiles'][version]['entry'], 16)
                        for name, row in config['cache_entries'].items()}
            self.assertEqual({name: actual.get(name) for name in expected}, expected,
                             'reviewed cache entries are missing from the generated table: ' + version)
    def test_stock_images_match_runtime_identity(self):
        maps = GEN.validate(self.config, self.images, ROOT / 'src')
        self.assertEqual(set(maps), {'azmod', 'azstem'})
        self.assertEqual(len(self.config['cache_entries']), 22)
    def test_dispatcher_target_profile_is_generated_from_the_same_mappings(self):
        headers = GEN.generate(ROOT, self.config, self.images)
        self.assertIn(ROOT / 'src/azmod/sdk_cache_targets.h', set(headers),
                      'dispatcher targets must use the checked mappings and signatures')
    def test_wrong_version_image_rejected(self):
        with self.assertRaisesRegex(GEN.MappingError, 'identity'):
            GEN.Image(self.firmware / 'EP147-1.30', self.config['builds']['2.00'])
    def test_wrong_firmware_hash_rejected(self):
        wrong = dict(self.config['builds']['1.30'], sha256='0' * 64)
        with self.assertRaisesRegex(GEN.MappingError, 'identity'):
            GEN.Image(self.firmware / 'EP147-1.30', wrong)
    def test_wrong_address_rejected(self):
        config = self.changed()
        row = config['cache_entries']['EP_124a7a8']['profiles']['2.00']
        row['entry'] = hex(int(row['entry'], 16) + 4)
        row['end'] = hex(int(row['end'], 16) + 4)
        with self.assertRaisesRegex(GEN.MappingError, 'signature'):
            GEN.validate(config, self.images, ROOT / 'src')
    def test_changed_signature_rejected(self):
        config = self.changed()
        config['cache_entries']['EP_124a7a8']['profiles']['1.30']['first16'] = '00' * 16
        with self.assertRaisesRegex(GEN.MappingError, 'signature'):
            GEN.validate(config, self.images, ROOT / 'src')
    def test_full_extent_is_checked_beyond_prologue(self):
        config = self.changed()
        row = config['cache_entries']['EP_124a7a8']['profiles']['2.00']
        row['end'] = hex(int(row['end'], 16) + 4)
        with self.assertRaisesRegex(GEN.MappingError, 'signature'):
            GEN.validate(config, self.images, ROOT / 'src')
    def test_nonexecuting_target_rejected(self):
        config = self.changed()
        row = config['cache_entries']['EP_124a7a8']['profiles']['2.00']
        base = next(b for b, _, size, _, flags in self.images['2.00'].loads if not flags & 1 and size >= 16)
        row['entry'], row['end'] = hex(base), hex(base + 16)
        with self.assertRaisesRegex(GEN.MappingError, 'executable'):
            GEN.validate(config, self.images, ROOT / 'src')
    def test_zero_and_missing_build_rejected(self):
        config = self.changed()
        config['cache_entries']['EP_124a7a8']['profiles']['2.00']['entry'] = '0x0'
        with self.assertRaisesRegex(GEN.MappingError, 'zero'):
            GEN.validate(config, self.images, ROOT / 'src')
        config = self.changed()
        del config['cache_entries']['EP_124a7a8']['profiles']['2.00']
        with self.assertRaisesRegex(GEN.MappingError, 'incomplete'):
            GEN.validate(config, self.images, ROOT / 'src')
    def test_source_scanning_ignores_comments_strings_and_generated_header(self):
        with tempfile.TemporaryDirectory() as folder:
            source = self.fixture(folder) / 'src'
            (source / 'azmod/use.c').write_text('/* EP_deadbeef */ const char *s="EP_123456"; // EP_11111\nlong f(void) { return EP_aabb78; }')
            (source / 'azmod/ep147addr.h').write_text('#define EP_bad EP_dead\n')
            self.assertEqual(GEN.used_names(source, 'azmod'), {'EP_aabb78'})
            with self.assertRaisesRegex(GEN.MappingError, 'missing both-build'):
                GEN.render('azmod', {'EP_deadbeef'}, {})
    def test_check_mode_detects_changed_output_and_removed_use(self):
        with tempfile.TemporaryDirectory() as folder:
            root = self.fixture(folder)
            self.assertEqual(self.cli(root), 0)
            self.assertEqual(self.cli(root, '--check'), 0)
            header = root / 'src/azmod/ep147addr.h'
            header.write_text(header.read_text() + '// hand edit\n')
            self.assertEqual(self.cli(root, '--check'), 1)
            self.assertEqual(self.cli(root), 0)
            (root / 'src/azmod/use.c').write_text('long f(void) { return 0; }\n')
            self.assertEqual(self.cli(root, '--check'), 1)
    def test_inline_assembly_cannot_bypass_the_address_table(self):
        with tempfile.TemporaryDirectory() as folder:
            root = self.fixture(folder)
            (root / 'src/azmod/use.c').write_text('long f(void) { return EP_aabb78; }\n'
                'void g(void) { __asm__ volatile("movz x16, #0xbb78; movk x16, #0xaa, lsl #16; blr x16"); }\n')
            with self.assertRaisesRegex(GEN.MappingError, 'assembly.*address'):
                GEN.generate(root, self.config, self.images)
    def test_failed_validation_leaves_both_outputs_untouched(self):
        with tempfile.TemporaryDirectory() as folder:
            root = self.fixture(folder)
            for lib in GEN.LIBRARIES:
                (root / 'src' / lib / 'ep147addr.h').write_text('untouched')
            config = self.changed()
            config['cache_entries']['EP_124a7a8']['profiles']['2.00']['first16'] = 'ff' * 16
            mapping = root / 'wrong.json'
            mapping.write_text(json.dumps(config))
            self.assertEqual(self.cli(root, '--mapping', str(mapping)), 2)
            for lib in GEN.LIBRARIES:
                self.assertEqual((root / 'src' / lib / 'ep147addr.h').read_text(), 'untouched')
    def test_excluded_boundaries_not_promoted(self):
        header = (ROOT / 'src/azmod/ep147addr.h').read_text()
        for name in ['EP_12b54d8', 'EP_123ef48', 'EP_123d9e8', 'EP_11fc7e0']:
            self.assertNotIn('#define ' + name + ' ', header)
    def test_required_island_modes_preserved(self):
        for name in ['EP_123d6c8', 'EP_1246c50']:
            self.assertEqual(self.config['cache_entries'][name]['hook'], 'island')
    def test_generated_dispatch_profile_has_exact_slots_bytes_and_build_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            source, binary = Path(folder) / 'profile.c', Path(folder) / 'profile'
            source.write_text('#define _POSIX_C_SOURCE 200809L\n#include <assert.h>\n#include <stdio.h>\n'
                '#include "' + str(ROOT / 'src/azmod/sdk_cache_targets.h') + '"\n'
                'int main(void) { struct sdk_cache_target out[SDK_CACHE_TARGET_COUNT];\n'
                'memset(out,0xa5,sizeof out); assert(!sdk_cache_target_profile(EPB_V130,out,SDK_CACHE_TARGET_COUNT));\n'
                'for(int b=1;b<=2;b++) { assert(ep147_addr_init(b));\n'
                'assert(!sdk_cache_target_profile(999,out,SDK_CACHE_TARGET_COUNT));\n'
                'assert(!sdk_cache_target_profile(b==1?2:1,out,SDK_CACHE_TARGET_COUNT));\n'
                'assert(!sdk_cache_target_profile(b,out,SDK_CACHE_TARGET_COUNT-1));\n'
                'assert(sdk_cache_target_profile(b,out,SDK_CACHE_TARGET_COUNT)==SDK_CACHE_TARGET_COUNT);\n'
                'unsigned mask=0; for(unsigned i=0;i<SDK_CACHE_TARGET_COUNT;i++) {\n'
                'assert(out[i].slot>0 && out[i].slot<AZCE_KIND_COUNT && !(mask&(1u<<out[i].slot))); mask|=1u<<out[i].slot;\n'
                'printf("%d %u %lx %u ",b,out[i].slot,(unsigned long)out[i].address,out[i].island_only);\n'
                'for(unsigned j=0;j<16;j++)printf("%02x",out[i].expected[j]); puts(""); }\n'
                'assert(mask==((1u<<AZCE_KIND_COUNT)-2)); } return 0; }\n')
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(source), '-o', str(binary)], check=True)
            output = subprocess.check_output([str(binary)], text=True)
            kinds = {name: int(value) for name, value in re.findall(r'^#define (AZCE_KIND_[A-Z_]+) (\d+)$', (ROOT / 'src/common/azcacheentry.h').read_text(), re.M)}
            expected = {(str(index + 1), str(kinds[row['kind']])):
                        (int(row['profiles'][version]['entry'], 16), int(row['hook'] == 'island'), row['profiles'][version]['first16'])
                        for row in self.config['cache_entries'].values() for index, version in enumerate(GEN.VERSIONS)}
            self.assertEqual(len(output.splitlines()), len(expected))
            for line in output.splitlines():
                build, slot, address, island, signature = line.split()
                self.assertEqual((int(address, 16), int(island), signature), expected[(build, slot)])
    def test_generated_c_contract_for_both_libraries_and_builds(self):
        with tempfile.TemporaryDirectory() as folder:
            for lib, name, first, second in [('azmod', '1243350', 0x1243350, 0x12feae0),
                                             ('azstem', '14a2560', 0x14a2560, 0x155d9f0)]:
                code = '#define _POSIX_C_SOURCE 200809L\n#include <assert.h>\n#include "' + str(ROOT / 'src' / lib / 'ep147addr.h') + '"\n'
                code += 'int main(void) { for(int i=0;i<EPI_N;i++) assert(!ep147_addr[i]); assert(!ep147_addr_init(999)); assert(ep147_addr_init(EPB_V130)); assert(EP_' + name + ' == ' + str(first) + '); assert(ep147_addr_init(EPB_V200)); assert(EP_' + name + ' == ' + str(second) + '); for(int i=0;i<EPI_N;i++) assert(ep147_addr[i]); return 0; }\n'
                source, binary = Path(folder) / (lib + '.c'), Path(folder) / lib
                source.write_text(code)
                subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

if __name__ == '__main__': unittest.main()
