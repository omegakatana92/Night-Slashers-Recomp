"""ARM2 / Data East DE156 instruction decoding.

The DE156 is an ARM2 core in 26-bit mode (MAME's arm2/DE156 device); there is no Thumb,
no halfword transfer, no BX, no long multiply. The decoder keeps the fields the lifter
and disassembler need; it does not execute anything.

MAME reference: src/devices/cpu/arm7/arm7.h (de156_cpu_device is an arm2_cpu_device with
ARCHFLAG_MODE26 | ARCHFLAG_ONLY26). The encoding is the ARM Architecture Reference Manual's
ARM instruction set, as ARM2 implements it.
"""
from __future__ import annotations

from dataclasses import dataclass

CONDITIONS = {
    0x0: ("eq", "Z"), 0x1: ("ne", "!Z"), 0x2: ("cs", "C"), 0x3: ("cc", "!C"),
    0x4: ("mi", "N"), 0x5: ("pl", "!N"), 0x6: ("vs", "V"), 0x7: ("vc", "!V"),
    0x8: ("hi", "C && !Z"), 0x9: ("ls", "!C || Z"), 0xA: ("ge", "N == V"),
    0xB: ("lt", "N != V"), 0xC: ("gt", "!Z && N == V"), 0xD: ("le", "Z || N != V"),
    0xE: ("", "1"), 0xF: ("nv", "0"),
}

DP_OPS = {
    0x0: "and", 0x1: "eor", 0x2: "sub", 0x3: "rsb", 0x4: "add", 0x5: "adc",
    0x6: "sbc", 0x7: "rsc", 0x8: "tst", 0x9: "teq", 0xA: "cmp", 0xB: "cmn",
    0xC: "orr", 0xD: "mov", 0xE: "bic", 0xF: "mvn",
}
DP_LOGICAL = {0x0, 0x1, 0x8, 0x9, 0xC, 0xD, 0xE, 0xF}
DP_ARITHMETIC = {0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xA, 0xB}
DP_TEST = {0x8, 0x9, 0xA, 0xB}
SHIFT_NAMES = ("lsl", "lsr", "asr", "ror")


class DecodeError(Exception):
    pass


@dataclass(frozen=True)
class Operand2:
    immediate: bool
    value: int = 0             # rotated 32-bit constant when immediate
    rotation: int = 0          # the rotate count * 2
    rm: int = 0
    shift: int = 0             # 0 lsl, 1 lsr, 2 asr, 3 ror
    shift_immediate: bool = True
    shift_amount: int = 0      # immediate shift, or register number when shift_immediate is False

    @property
    def uses_pc(self) -> bool:
        if self.immediate:
            return False
        return self.rm == 15 or (not self.shift_immediate and self.shift_amount == 15)


@dataclass(frozen=True)
class Instruction:
    address: int
    word: int
    cond: int
    kind: str
    target: int | None = None
    branch_link: bool = False
    rn: int = 0
    rd: int = 0
    rs: int = 0
    rm: int = 0
    opcode: int = 0
    set_flags: bool = False
    immediate: bool = False
    value: int = 0
    offset: int = 0
    up: bool = True
    pre_indexed: bool = True
    writeback: bool = False
    load: bool = False
    byte: bool = False
    register_offset: bool = False
    reg_list: int = 0
    cp_number: int = 0
    cp_opcode: int = 0
    cp_creg: int = 0
    cp_creg2: int = 0
    cp_rm: int = 0
    swi_number: int = 0
    operand2: Operand2 | None = None
    undefined: bool = False

    @property
    def condition(self) -> str:
        return CONDITIONS[self.cond][0]

    @property
    def condition_c(self) -> str:
        return CONDITIONS[self.cond][1]

    @property
    def next_address(self) -> int:
        return (self.address + 4) & 0x3FFFFFF

    @property
    def pc_value(self) -> int:
        """The value a read of R15 gives in this instruction (ARM pipelining)."""
        return (self.address + 8) & 0x3FFFFFF

    @property
    def writes_pc(self) -> bool:
        if self.kind == "branch":
            return True
        if self.kind == "dp":
            if self.opcode in DP_TEST:
                return False          # TSTP/TEQP/CMPP/CMNP write PSR only
            return self.rd == 15
        if self.kind == "ldr":
            return self.load and self.rd == 15
        if self.kind == "ldm":
            return self.load and bool(self.reg_list & 0x8000)
        return False

    @property
    def is_psr_write(self) -> bool:
        return self.kind == "dp" and self.set_flags and self.rd == 15 and self.opcode not in DP_TEST

    @property
    def is_psr_test(self) -> bool:
        return self.kind == "dp" and self.set_flags and self.rd == 15 and self.opcode in DP_TEST


def _sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def _decode_operand2(word: int) -> Operand2:
    if word & (1 << 25):
        immediate = word & 0xFF
        rotation = ((word >> 8) & 0xF) * 2
        value = ((immediate >> rotation) | (immediate << (32 - rotation))) & 0xFFFFFFFF if rotation else immediate
        return Operand2(True, value=value, rotation=rotation)
    rm = word & 0xF
    if word & (1 << 4):
        shift = (word >> 5) & 3
        rs = (word >> 8) & 0xF
        return Operand2(False, rm=rm, shift=shift, shift_immediate=False, shift_amount=rs)
    shift = (word >> 5) & 3
    amount = (word >> 7) & 0x1F
    return Operand2(False, rm=rm, shift=shift, shift_immediate=True, shift_amount=amount)


def decode(word: int, address: int = 0) -> Instruction:
    word &= 0xFFFFFFFF
    address &= 0x3FFFFFF
    cond = (word >> 28) & 0xF
    base = dict(address=address, word=word, cond=cond)

    if (word & 0x0FFFFFF0) == 0x012FFF10:
        return Instruction(**base, kind="bx", rm=word & 0xF, undefined=True)

    if (word & 0x0C000000) == 0x00000000:
        if (word & 0x0FC000F0) == 0x00000090:
            # MUL / MLA (ARMv2 has 32-bit only)
            accumulate = bool(word & (1 << 21))
            set_flags = bool(word & (1 << 20))
            return Instruction(**base, kind="mul", rd=(word >> 16) & 0xF,
                               rn=(word >> 12) & 0xF, rs=(word >> 8) & 0xF,
                               rm=word & 0xF, set_flags=set_flags,
                               opcode=1 if accumulate else 0)
        if (word & 0x0FBF0FFF) == 0x010F0000:
            # MRS Rd, CPSR (ARMv3; present in ARM2 as a data-processing alias, decoded here
            # so an unexpected use is reported rather than silently mis-executed)
            return Instruction(**base, kind="mrs", rd=(word >> 12) & 0xF,
                               opcode=1 if word & (1 << 22) else 0, undefined=True)
        if (word & 0x0FB0FFF0) == 0x0120F000:
            # MSR CPSR, Rm (ARMv3 alias)
            return Instruction(**base, kind="msr", rm=word & 0xF,
                               opcode=1 if word & (1 << 22) else 0, undefined=True)
        opcode = (word >> 21) & 0xF
        set_flags = bool(word & (1 << 20))
        rn = (word >> 16) & 0xF
        rd = (word >> 12) & 0xF
        operand2 = _decode_operand2(word)
        return Instruction(**base, kind="dp", opcode=opcode, set_flags=set_flags,
                           rn=rn, rd=rd, operand2=operand2)

    if (word & 0x0C000000) == 0x04000000:
        register_offset = bool(word & (1 << 25))
        pre = bool(word & (1 << 24))
        up = bool(word & (1 << 23))
        byte = bool(word & (1 << 22))
        writeback = bool(word & (1 << 21))
        load = bool(word & (1 << 20))
        rn = (word >> 16) & 0xF
        rd = (word >> 12) & 0xF
        offset = word & 0xFFF
        return Instruction(**base, kind="ldr", register_offset=register_offset,
                           pre_indexed=pre, up=up, byte=byte, writeback=writeback,
                           load=load, rn=rn, rd=rd, offset=offset,
                           rm=word & 0xF,
                           operand2=(Operand2(False, rm=word & 0xF,
                                              shift=(word >> 5) & 3,
                                              shift_immediate=True,
                                              shift_amount=(word >> 7) & 0x1F)
                                     if register_offset else None))

    if (word & 0x0E000000) == 0x08000000:
        pre = bool(word & (1 << 24))
        up = bool(word & (1 << 23))
        psr_or_user = bool(word & (1 << 22))
        writeback = bool(word & (1 << 21))
        load = bool(word & (1 << 20))
        rn = (word >> 16) & 0xF
        reg_list = word & 0xFFFF
        return Instruction(**base, kind="ldm", pre_indexed=pre, up=up,
                           set_flags=psr_or_user, writeback=writeback, load=load,
                           rn=rn, reg_list=reg_list)

    if (word & 0x0E000000) == 0x0A000000:
        offset = _sign_extend(word & 0xFFFFFF, 24) << 2
        target = (address + 8 + offset) & 0x3FFFFFF
        return Instruction(**base, kind="branch", target=target,
                           branch_link=bool(word & (1 << 24)),
                           offset=offset)

    if (word & 0x0F000000) == 0x0F000000:
        return Instruction(**base, kind="swi", swi_number=word & 0xFFFFFF)

    if (word & 0x0E000000) == 0x0C000000:
        # LDC/STC: coprocessor load/store
        return Instruction(**base, kind="ldc", load=bool(word & (1 << 20)),
                           rn=(word >> 16) & 0xF, rd=(word >> 12) & 0xF,
                           cp_number=(word >> 8) & 0xF, offset=word & 0xFF,
                           pre_indexed=bool(word & (1 << 24)), up=bool(word & (1 << 23)),
                           writeback=bool(word & (1 << 21)),
                           byte=bool(word & (1 << 22)), undefined=True)

    if (word & 0x0F000000) == 0x0E000000:
        cp_number = (word >> 8) & 0xF
        cp_opcode = (word >> 20) & 0xF
        cp_creg = (word >> 12) & 0xF
        cp_creg2 = (word >> 16) & 0xF
        if word & (1 << 4):
            # MCR / MRC
            load = bool(word & (1 << 20))
            return Instruction(**base, kind="mrc" if load else "mcr",
                               cp_number=cp_number, cp_opcode=cp_opcode,
                               cp_creg=cp_creg, cp_creg2=cp_creg2,
                               rd=(word >> 12) & 0xF, cp_rm=word & 0xF,
                               load=load)
        return Instruction(**base, kind="cdp", cp_number=cp_number,
                           cp_opcode=cp_opcode, cp_creg=cp_creg,
                           cp_creg2=cp_creg2, rd=(word >> 12) & 0xF)

    return Instruction(**base, kind="undefined", undefined=True)
