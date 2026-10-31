"""Disassemble ARM2 / DE156 code."""
from __future__ import annotations

from .decode import (CONDITIONS, DP_OPS, SHIFT_NAMES, Instruction, Operand2, decode)
from . import decode as dec


def _reg(number: int) -> str:
    return {13: "sp", 14: "lr", 15: "pc"}.get(number, f"r{number}")


def _operand2(operand: Operand2) -> str:
    if operand.immediate:
        return f"#{operand.value:#x}"
    text = _reg(operand.rm)
    if operand.shift_immediate:
        amount = operand.shift_amount
        if operand.shift == 0 and amount == 0:
            return text
        if operand.shift == 3 and amount == 0:
            return f"rrx {text}"
        return f"{text}, {SHIFT_NAMES[operand.shift]} #{amount}"
    return f"{text}, {SHIFT_NAMES[operand.shift]} {_reg(operand.shift_amount)}"


def _reglist(mask: int) -> str:
    names = [_reg(index) for index in range(16) if mask & (1 << index)]
    return "{" + ", ".join(names) + "}"


def _condition(instruction: Instruction) -> str:
    name = CONDITIONS[instruction.cond][0]
    return "." + name if name else ""


def text(instruction: Instruction) -> str:
    cond = _condition(instruction)
    kind = instruction.kind
    if kind == "dp":
        mnemonic = DP_OPS[instruction.opcode]
        if mnemonic in ("mov", "mvn"):
            body = f"{_reg(instruction.rd)}, {_operand2(instruction.operand2)}"
        elif mnemonic in ("tst", "teq", "cmp", "cmn"):
            body = f"{_reg(instruction.rn)}, {_operand2(instruction.operand2)}"
        else:
            body = (f"{_reg(instruction.rd)}, {_reg(instruction.rn)}, "
                    f"{_operand2(instruction.operand2)}")
        bare = mnemonic + ("s" if instruction.set_flags else "") + cond
        if instruction.set_flags and instruction.rd == 15:
            bare = mnemonic + "p" + cond if mnemonic in ("tst", "teq", "cmp", "cmn") else mnemonic + "s" + cond
        return f"{bare} {body}"
    if kind == "mul":
        mnemonic = ("mla" if instruction.opcode else "mul") + ("s" if instruction.set_flags else "") + cond
        if instruction.opcode:
            return f"{mnemonic} {_reg(instruction.rd)}, {_reg(instruction.rm)}, {_reg(instruction.rs)}, {_reg(instruction.rn)}"
        return f"{mnemonic} {_reg(instruction.rd)}, {_reg(instruction.rm)}, {_reg(instruction.rs)}"
    if kind == "ldr":
        mnemonic = ("ldr" if instruction.load else "str") + ("b" if instruction.byte else "") + cond
        base = _reg(instruction.rn)
        if instruction.register_offset:
            offset = _operand2(instruction.operand2)
            offset = offset if offset.startswith(("r", "#")) else offset
        else:
            offset = f"#{instruction.offset:#x}"
        if instruction.pre_indexed:
            sign = "" if instruction.up else "-"
            address = f"[{base}, {sign}{offset}]"
            if instruction.writeback:
                address += "!"
        else:
            sign = "" if instruction.up else "-"
            address = f"[{base}], {sign}{offset}"
            if instruction.writeback:
                address += "!"
        return f"{mnemonic} {_reg(instruction.rd)}, {address}"
    if kind == "ldm":
        mnemonic = ("ldm" if instruction.load else "stm") + cond
        mode = {(True, True): "ib", (True, False): "db",
                (False, True): "ia", (False, False): "da"}[(instruction.pre_indexed, instruction.up)]
        base = _reg(instruction.rn) + ("!" if instruction.writeback else "")
        caret = "^" if instruction.set_flags else ""
        return f"{mnemonic}{mode} {base}, {_reglist(instruction.reg_list)}{caret}"
    if kind == "branch":
        mnemonic = ("bl" if instruction.branch_link else "b") + cond
        return f"{mnemonic} #{instruction.target:#x}"
    if kind == "swi":
        return f"swi{cond} #{instruction.swi_number:#x}"
    if kind == "mrc":
        return f"mrc{cond} p{instruction.cp_number}, {instruction.cp_opcode}, {_reg(instruction.rd)}, c{instruction.cp_creg}, c{instruction.cp_creg2}"
    if kind == "mcr":
        return f"mcr{cond} p{instruction.cp_number}, {instruction.cp_opcode}, {_reg(instruction.rd)}, c{instruction.cp_creg}, c{instruction.cp_creg2}"
    if kind == "cdp":
        return f"cdp{cond} p{instruction.cp_number}, {instruction.cp_opcode}, c{instruction.cp_creg}, c{instruction.cp_creg2}, c{instruction.rd}"
    if kind == "undefined":
        return f".word {instruction.word:#010x}"
    if kind == "bx":
        return f"bx{cond} {_reg(instruction.rm)}  ; not ARM2"
    return f".word {instruction.word:#010x}  ; {kind}"


def disassemble(image: bytes, start: int = 0, end: int | None = None) -> list[tuple[int, str]]:
    end = len(image) if end is None else min(end, len(image))
    result = []
    for address in range(start & ~3, end, 4):
        word = int.from_bytes(image[address:address + 4], "little")
        result.append((address, text(decode(word, address))))
    return result
