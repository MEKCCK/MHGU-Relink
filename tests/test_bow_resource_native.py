"""Bounded execution of original ARM32 Bow resource setup and slot-2 dispatch.

The production C hook supplies its results through --contract. This probe does
not implement the fix, replace slot writes, or execute trajectory/render code.
Unicorn is a host-test dependency only; set MHGU_UNICORN_PATH if needed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'tools'))
if os.environ.get('MHGU_UNICORN_PATH'):
    sys.path.insert(0, os.environ['MHGU_UNICORN_PATH'])
from nso import Nso
import unicorn
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_CPSR, UC_ARM_REG_FPEXC, UC_ARM_REG_C1_C0_2,
)

BASE = 0x200000
MAIN_SHA256 = 'b3935415499549adaf4c0e89822831daa175cfd456a7990791c79be3997645ef'
BUILD_ID = 'fb08f1d20fd1204fe7070672777f869b'
EFFECT, LINK, HUNTER, DATA = 0x08001000, 0x08003000, 0x08004000, 0x08008000
SLOTS, TYPES, STACK, RETURN = 0x0800a000, 0x0800a040, 0x09000000, 0x0f000000
EFFECT_VTABLE, BOW_VTABLE = 0x195e160, 0x194e9e0
SLOT_COUNT = 7  # Original uShellPlEffectW10 factory: mov r1,#7 at 0x656a4c.
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class NativeProbe:
    def __init__(self, nso, contract):
        self.contract = {(row['style'], row['parameter']): row['result']
                         for row in contract['style_results']}
        assert len(self.contract) == 32
        self.aim = contract['aim_results']
        assert self.aim == {'charging': 1, 'idle': 0, 'sheathed': 0, 'manual': 1}
        image = bytearray(nso.image)
        for offset, kind, _symbol, addend in nso.relocs:
            if kind == 23:  # The native image's R_ARM_RELATIVE vtables/data.
                struct.pack_into('<I', image, offset, (BASE + addend) & 0xffffffff)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map(BASE, nso.size)
        self.uc.mem_write(BASE, bytes(image))
        assert self.word(0x656a4c) == 0xe3a01007
        self.uc.mem_map(0x08000000, 0x20000)
        self.uc.mem_map(STACK, 0x20000)
        self.uc.mem_map(RETURN, 0x1000)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0x00f00000)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        for offset, address in [(0x130, 0x26be88), (0x148, 0x656aa8),
                                (0x158, 0x64cdd0), (0x160, 0x656d00)]:
            assert self.word(EFFECT_VTABLE + offset) == address
        for offset, address in [(0x15c, 0x47c708), (0x168, 0x468818)]:
            assert self.word(BOW_VTABLE + offset) == address
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write)
        self.allowed = [
            (0x656aa8, 0x656ce4), (0x64c888, 0x64c8f0),
            (0x6a1b6c, 0x6a1ba4), (0x47bcd0, 0x47bce0),
            (0x26be88, 0x26be90), (0x468818, 0x468828),
            (0x47c708, 0x47c718), (0x2e173c, 0x2e174c),
            (0x64cdd0, 0x64ce7c), (0x64cb34, 0x64cb68),
            (0x656d00, 0x656d44), (0x656ee8, 0x656f08),
        ]

    def word(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def put(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value))

    def reg(self, number):
        return self.uc.reg_read(REGS[number])

    def ret(self, result=None):
        if result is not None:
            self.uc.reg_write(UC_ARM_REG_R0, result)
        self.uc.reg_write(UC_ARM_REG_PC, self.reg(14))

    def write(self, _uc, _access, address, size, value, _data):
        if SLOTS <= address < SLOTS + SLOT_COUNT * 4 or TYPES <= address < TYPES + SLOT_COUNT:
            self.writes.append({'pc': hex(self.reg(15)), 'address': hex(address),
                                'size': size, 'value': value})

    def code(self, _uc, address, _size, _data):
        self.instructions += 1
        if self.phase == 'setup':
            if address == 0x240b10:  # Descriptor ctor: resource placement is out of scope.
                self.stubs.append('descriptor_ctor')
                self.uc.mem_write(self.reg(0), bytes(0xe0))
                self.ret()
                return
            if address == 0x240b50:
                self.stubs.append('descriptor_dtor')
                self.ret()
                return
            if address == 0x6a2f5c:
                assert self.reg(0) == EFFECT and self.reg(1) == 0
                effect_type = self.reg(2)
                self.resolved.append(effect_type)
                handle = 0 if self.failure == 'resolve' and effect_type != 4 else 0x4000 + effect_type
                self.ret(handle)
                return
            if address == 0x6a1e50:
                assert self.reg(0) == EFFECT
                effect_type = self.reg(1) - 0x4000
                assert effect_type in self.resolved
                self.allocated.append(effect_type)
                pointer = 0 if self.failure == 'allocate' and effect_type != 4 else 0x08010000 + effect_type * 0x400
                self.ret(pointer)
                return
            if address == 0x656c78:
                assert self.reg(0) == self.style and self.reg(14) == address
                self.native_style = self.reg(0)
                if self.fixed:
                    before = [self.reg(i) for i in range(16)]
                    self.uc.reg_write(UC_ARM_REG_R0, self.contract[self.style, self.parameter])
                    after = [self.reg(i) for i in range(16)]
                    assert after[1:] == before[1:]
                self.effective_style = self.reg(0)
            if address == 0x656ca4:
                assert self.reg(0) == self.parameter
                self.native_parameter = self.reg(0)
        else:
            if address == 0x64cc20:
                assert self.word(SLOTS + 8) == 0 and not self.aim_reached
                self.stop = 'null_slot_skipped'
                self.uc.emu_stop()
                return
            if address == 0x13b7750:
                assert self.reg(0) == HUNTER and self.reg(14) == 0x656ef0
                assert self.reg(7) == self.uc.mem_read(TYPES + 2, 1)[0]
                assert self.reg(10) == self.word(SLOTS + 8)
                self.aim_reached = True
                self.ret(self.aim[self.mode])
                return
            if address in (0x13b9568, 0x65794c):
                assert self.aim_reached
                self.stop = 'native_charge_level_gate' if address == 0x13b9568 else 'native_visibility_rejected'
                self.uc.emu_stop()
                return
        assert any(lo <= address < hi for lo, hi in self.allowed), f'Unexpected native PC {address:#x}'

    def run(self, start):
        self.instructions = 0
        self.uc.emu_start(start, RETURN, count=5000)
        assert self.instructions < 5000, 'Native execution exceeded bounded instruction count'
        return self.instructions

    def fixture(self, style, parameter, fixed, local=1, failure=None):
        self.style, self.parameter, self.fixed, self.local, self.failure = style, parameter, fixed, local, failure
        self.writes, self.stubs, self.resolved, self.allocated = [], [], [], []
        self.native_style = self.effective_style = self.native_parameter = None
        self.uc.mem_write(0x08000000, bytes(0x20000))
        self.uc.mem_write(STACK, bytes(0x20000))
        for reg in REGS:
            self.uc.reg_write(reg, 0)
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x10)
        self.put(EFFECT, EFFECT_VTABLE)
        self.put(EFFECT + 0x136c, LINK)
        self.put(LINK + 0xc, HUNTER)
        self.put(HUNTER, BOW_VTABLE)
        self.put(HUNTER + 0xc, 1)
        self.put(HUNTER + 0x13cc, local)
        self.put(HUNTER + 0x13c4, DATA)
        self.uc.mem_write(DATA + 0x4d9, bytes([style]))
        self.uc.mem_write(DATA + 0x356, struct.pack('<h', parameter))
        self.put(EFFECT + 0x15a4, SLOT_COUNT)
        self.put(EFFECT + 0x15a8, SLOTS)
        self.put(EFFECT + 0x15ac, TYPES)
        self.uc.mem_write(TYPES, bytes([0xff]) * SLOT_COUNT)
        self.hunter_before = bytes(self.uc.mem_read(HUNTER, 0x5000))
        self.phase = 'setup'
        self.uc.reg_write(UC_ARM_REG_R0, EFFECT)
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x1f000)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN)
        setup_instructions = self.run(0x656aa8)
        assert self.reg(15) == RETURN
        has_resource = self.word(SLOTS + 8) != 0
        expected_native = style in (0, 1, 4)
        expected_added = fixed and style in (2, 3, 5) and parameter < 3
        expect_resource = bool(local & 1 and (expected_native or expected_added) and not failure)
        assert has_resource == expect_resource, (style, parameter, fixed, local, failure, hex(self.word(SLOTS + 8)))
        assert bool(self.word(SLOTS + 4)) == bool(local & 1)
        if local & 1:
            assert self.native_style == style
        if has_resource:
            expected_type = (6, 7, 5)[parameter]
            assert self.native_parameter == parameter
            assert self.uc.mem_read(TYPES + 2, 1)[0] == expected_type
            assert self.word(SLOTS + 8) == 0x08010000 + expected_type * 0x400
            assert self.uc.mem_read(self.word(SLOTS + 8) + 0x1c1, 1)[0] == 0
            assert {'pc': '0x64ce60', 'address': hex(SLOTS + 8), 'size': 4,
                    'value': self.word(SLOTS + 8)} in self.writes
            assert {'pc': '0x64ce6c', 'address': hex(TYPES + 2), 'size': 1,
                    'value': expected_type} in self.writes
        outcomes = {}
        for mode in self.aim:
            self.phase, self.mode, self.stop, self.aim_reached = 'update', mode, None, False
            for reg in REGS:
                self.uc.reg_write(reg, 0)
            for number, value in {0: SLOTS, 5: 2, 6: EFFECT + 0x15a8,
                                  7: EFFECT + 0x15a4, 8: EFFECT,
                                  10: 1, 11: EFFECT + 0x15ac,
                                  13: STACK + 0x1f000, 14: RETURN}.items():
                self.uc.reg_write(REGS[number], value)
            count = self.run(0x64cb34)
            expected_stop = ('native_charge_level_gate' if self.aim[mode]
                             else 'native_visibility_rejected') if has_resource else 'null_slot_skipped'
            assert self.stop == expected_stop and self.aim_reached == has_resource
            outcomes[mode] = {'stop': self.stop, 'aim_lr': '0x656ef0' if self.aim_reached else None,
                              'instructions': count}
        assert bytes(self.uc.mem_read(HUNTER, 0x5000)) == self.hunter_before
        return {'style': style, 'parameter': parameter, 'fixed': fixed, 'local': local,
                'failure': failure, 'native_style': self.native_style,
                'effective_style': self.effective_style, 'native_parameter': self.native_parameter,
                'slot2_pointer': hex(self.word(SLOTS + 8)),
                'slot2_type': self.uc.mem_read(TYPES + 2, 1)[0],
                'native_writes': list(self.writes), 'resolved_types': list(self.resolved),
                'allocated_types': list(self.allocated), 'descriptor_stubs': list(self.stubs),
                'setup_instructions': setup_instructions, 'updates': outcomes,
                'hunter_and_style_memory_unchanged': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', type=Path, required=True)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {
        'contract': 'bow-native-landing-resource-v1', 'passed': False,
        'native_main': str(args.main), 'native_main_sha256': sha256(args.main),
        'test_sha256': sha256(__file__),
        'production_sha256': sha256(PROJECT / 'runtime/hle/bow_reticle.c'),
        'c_hook_contract_sha256': sha256(args.contract), 'unicorn_version': unicorn.__version__,
        'native_effect_slot_count': SLOT_COUNT,
        'method': 'Original relocated ARM32 instructions execute setup, local/owner/style/parameter getters, native slot writes and selected slot-2 update. Actual production C-hook outputs are substituted only at the two hook boundaries.',
        'limits': [
            'Descriptor constructor/destructor, resource resolver and allocator are fixture stubs; actual graphics assets are not loaded.',
            'The unrelated hunter+0x23a0 weapon visual attachment is absent in this fixture.',
            'Update starts at the native slot-2 loop body and stops at the next charge-level predicate or the native false-visibility branch.',
            'Native charge-level, trajectory, collision and rendering are not executed; this is not an APK or device/visual acceptance test.',
        ],
        'cases': [],
    }
    try:
        assert report['native_main_sha256'] == MAIN_SHA256, 'Unexpected original NSO'
        nso = Nso(str(args.main))
        assert nso.build_id == BUILD_ID, 'Unexpected original build ID'
        report['native_build_id'] = nso.build_id
        contract = json.loads(args.contract.read_text(encoding='utf-8'))
        probe = NativeProbe(nso, contract)
        for fixed in (False, True):
            for style in range(8):
                for parameter in range(3):
                    report['cases'].append(probe.fixture(style, parameter, fixed))
        for style in (2, 3, 5):
            report['cases'].append(probe.fixture(style, 3, True))
        for fixed in (False, True):
            for style in range(6):
                report['cases'].append(probe.fixture(style, 1, fixed, local=0))
        for style in (0, 1, 2, 3, 4, 5):
            for parameter in range(3):
                for failure in ('resolve', 'allocate'):
                    report['cases'].append(probe.fixture(style, parameter, True, failure=failure))
        report['case_count'] = len(report['cases'])
        report['update_count'] = sum(len(case['updates']) for case in report['cases'])
        report['baseline_missing_new_style_cases'] = sum(
            not case['fixed'] and case['local'] == 1 and case['style'] in (2, 3, 5)
            and case['slot2_pointer'] == '0x0' for case in report['cases'])
        assert report['baseline_missing_new_style_cases'] == 9
        report['passed'] = True
        print(f"PASS: native landing resource: {report['case_count']} setups, {report['update_count']} slot updates; original 2/3/5 reproduce 9 missing resources; fixed valid styles create native types 6/7/5 and reach LR 0x656ef0.")
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
