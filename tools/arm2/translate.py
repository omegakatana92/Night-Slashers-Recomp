"""Translate the analysed DE156 ARM2 program into native C.

Each basic block becomes one C function. Blocks end by setting nsl_cpu.pc and returning
to the dispatcher; no instruction is decoded at run time. The generated C calls the
small helpers in runtime/src/cpu.c for flag-exact ALU operations, shifts, exceptions,
and the DE156 coprocessor. Nothing generated here may be redistributed: it is the
game's code, translated from the player's own ROM.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .analysis import Analysis
from .decode import DP_OPS, Instruction, SHIFT_NAMES
from .disassembler import text

NSL_MASK26 = 0x03FFFFFC
CONDITION_EXPR = {
    0x0: "nsl_cpu.z", 0x1: "!nsl_cpu.z", 0x2: "nsl_cpu.c", 0x3: "!nsl_cpu.c",
    0x4: "nsl_cpu.n", 0x5: "!nsl_cpu.n", 0x6: "nsl_cpu.v", 0x7: "!nsl_cpu.v",
    0x8: "nsl_cpu.c && !nsl_cpu.z", 0x9: "!nsl_cpu.c || nsl_cpu.z",
    0xA: "nsl_cpu.n == nsl_cpu.v", 0xB: "nsl_cpu.n != nsl_cpu.v",
    0xC: "!nsl_cpu.z && nsl_cpu.n == nsl_cpu.v", 0xD: "nsl_cpu.z || nsl_cpu.n != nsl_cpu.v",
    0xE: "1", 0xF: "0",
}


def reg(number: int) -> str:
    return f"nsl_cpu.r[{number}]"


def u32(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:08X}u"


class EmitError(Exception):
    pass


class BlockWriter:
    def __init__(self, program: "Program", block):
        self.program = program
        self.block = block
        self.lines: list[str] = []
        self.address = block.start

    def line(self, text: str) -> None:
        self.lines.append(text)

    def operand2(self, instruction: Instruction, set_flags: bool) -> str:
        operand = instruction.operand2
        if operand is None:
            raise EmitError(f"{instruction.address:06X}: no operand2")
        if operand.immediate:
            return u32(operand.value)
        if operand.shift_immediate:
            source = reg(operand.rm) if operand.rm != 15 else f"nsl_r15(0x{instruction.pc_value:06X}u)"
            if operand.shift == 0 and operand.shift_amount == 0:
                return source
            return f"nsl_shift_imm({source}, {operand.shift}, {operand.shift_amount})"
        source = reg(operand.rm) if operand.rm != 15 else f"nsl_r15(0x{instruction.pc_value:06X}u)"
        amount = reg(operand.shift_amount) if operand.shift_amount != 15 else f"nsl_r15(0x{instruction.pc_value:06X}u)"
        return f"nsl_shift_reg({source}, {operand.shift}, {amount})"

    def logical_carry(self, instruction: Instruction) -> str:
        operand = instruction.operand2
        if operand is None:
            return "nsl_cpu.c"
        if operand.immediate:
            return f"({(operand.value >> 31) & 1}u)" if operand.rotation else "nsl_cpu.c"
        return "nsl_cpu.shift_carry"

    def condition(self, instruction: Instruction) -> str | None:
        if instruction.cond == 0xE:
            return None
        return CONDITION_EXPR[instruction.cond]

    def emit_dp(self, insn: Instruction) -> None:
        name = DP_OPS[insn.opcode]
        is_test = insn.opcode in (0x8, 0x9, 0xA, 0xB)
        op2 = self.operand2(insn, insn.set_flags)
        result = f"t{self.address & 0xFFFF:x}"
        body: list[str] = []
        body.append(f"uint32_t {result};")
        operand = insn.operand2
        if operand is not None and not operand.immediate:
            body.append("nsl_cpu.shift_carry = nsl_cpu.c;")
        if insn.set_flags and insn.opcode in (0x0, 0x1, 0x8, 0x9, 0xC, 0xD, 0xE, 0xF):
            body.append("nsl_cpu.shift_carry = nsl_cpu.c;")
        rn = reg(insn.rn) if insn.rn != 15 else u32(insn.pc_value)
        if name in ("and", "eor", "orr", "bic"):
            op = {"and": "&", "eor": "^", "orr": "|", "bic": "& ~"}[name]
            expr = f"{rn} {op} ({op2})"
            body.append(f"{result} = nsl_logical({expr}, {1 if insn.set_flags else 0}, "
                        f"{self.logical_carry(insn)});")
        elif name in ("tst", "teq"):
            op = "&" if name == "tst" else "^"
            body.append(f"{result} = nsl_logical({rn} {op} ({op2}), 1, {self.logical_carry(insn)});")
        elif name == "mov":
            body.append(f"{result} = nsl_logical({op2}, {1 if insn.set_flags else 0}, "
                        f"{self.logical_carry(insn)});")
        elif name == "mvn":
            body.append(f"{result} = nsl_logical(~({op2}), {1 if insn.set_flags else 0}, "
                        f"{self.logical_carry(insn)});")
        elif name in ("add", "adc", "sub", "sbc", "rsb", "rsc"):
            carry = "nsl_cpu.c" if name in ("adc", "sbc", "rsc") else "0"
            flags = 1 if insn.set_flags else 0
            if name in ("add", "adc"):
                body.append(f"{result} = nsl_add({rn}, {op2}, {carry}, {flags});")
            elif name in ("sub", "sbc"):
                body.append(f"{result} = nsl_sub({rn}, {op2}, {carry}, {flags});")
            else:
                body.append(f"{result} = nsl_sub({op2}, {rn}, {carry}, {flags});")
        elif name in ("cmp", "cmn"):
            if name == "cmp":
                body.append(f"{result} = nsl_sub({rn}, {op2}, 0, 1);")
            else:
                body.append(f"{result} = nsl_add({rn}, {op2}, 0, 1);")
            body.append(f"(void){result};")
        else:
            raise EmitError(f"{insn.address:06X}: data processing {name}")

        if is_test:
            if insn.rd == 15:
                body.append(f"nsl_psr_test({result});")
            else:
                body.append(f"(void){result};")
        elif insn.rd == 15:
            if insn.set_flags:
                body.append(f"nsl_write_pc_and_psr({result});")
            else:
                body.append(f"nsl_cpu.pc = {result} & 0x03FFFFFCu;")
            body.append("return;")
        else:
            body.append(f"{reg(insn.rd)} = {result};")
        self.lines.extend(self.wrap(insn, body))
        if insn.rd == 15 and insn.cond != 0xE:
            self.lines.append(f"nsl_cpu.pc = 0x{insn.next_address:06X}u;")
            self.lines.append("return;")

    def wrap(self, insn: Instruction, body: list[str]) -> list[str]:
        condition = self.condition(insn)
        if condition is None:
            return body
        return [f"if ({condition}) {{"] + ["\t" + line for line in body] + ["}"]

    def address_operand(self, insn: Instruction) -> tuple[str, str | None]:
        """C address expression and a post-index writeback expression if needed."""
        if insn.rn == 15:
            base = f"0x{insn.pc_value:06X}u"
        else:
            base = reg(insn.rn)
        if insn.register_offset:
            offset = self.operand2(insn, False)
        else:
            offset = f"0x{insn.offset:X}u"
        if insn.pre_indexed:
            sign = "+" if insn.up else "-"
            address = f"(({base} {sign} {offset}) & 0x03FFFFFCu)"
            writeback = f"{reg(insn.rn)} = ({base} {'+' if insn.up else '-'} {offset}) & 0x03FFFFFCu;" if insn.writeback else None
            return address, writeback
        address = f"({base} & 0x03FFFFFCu)"
        writeback = f"{reg(insn.rn)} = ({base} {'+' if insn.up else '-'} {offset}) & 0x03FFFFFCu;"
        return address, writeback

    def emit_ldr(self, insn: Instruction) -> None:
        address, writeback = self.address_operand(insn)
        size = "8" if insn.byte else "32"
        body: list[str] = []
        if insn.load:
            if insn.rd == 15:
                body.append(f"nsl_cpu.pc = nsl_read{size}({address}) & 0x03FFFFFCu;")
            else:
                body.append(f"{reg(insn.rd)} = nsl_read{size}({address});")
            if writeback:
                body.append(writeback)
            if insn.rd == 15:
                body.append("return;")
        else:
            value = f"nsl_r15(0x{(insn.pc_value + 4):06X}u)" if insn.rd == 15 else reg(insn.rd)
            body.append(f"nsl_write{size}({address}, {value});")
            if writeback:
                body.append(writeback)
        self.lines.extend(self.wrap(insn, body))
        if insn.load and insn.rd == 15 and insn.cond != 0xE:
            self.lines.append(f"nsl_cpu.pc = 0x{insn.next_address:06X}u;")
            self.lines.append("return;")

    def emit_ldm(self, insn: Instruction) -> None:
        base = reg(insn.rn) if insn.rn != 15 else f"nsl_r15(0x{insn.pc_value:06X}u)"
        result = f"b{insn.address & 0xFFFF:x}"
        body = [
            f"uint32_t {result};",
            f"{result} = nsl_ldm({base}, 0x{insn.reg_list:04X}u, {1 if insn.load else 0}, "
            f"{1 if insn.pre_indexed else 0}, {1 if insn.up else 0}, {1 if insn.writeback else 0}, "
            f"{1 if insn.set_flags else 0}, 0x{insn.pc_value:06X}u, {insn.rn}u);",
        ]
        if insn.writeback and not (insn.load and (insn.reg_list & (1 << insn.rn))):
            body.append(f"{base} = {result};")
        if insn.load and (insn.reg_list & 0x8000):
            body.append("return;")
        else:
            body.append(f"nsl_cpu.pc = 0x{insn.next_address:06X}u;")
        self.lines.extend(self.wrap(insn, body))
        if insn.load and (insn.reg_list & 0x8000) and insn.cond != 0xE:
            self.lines.append(f"nsl_cpu.pc = 0x{insn.next_address:06X}u;")
            self.lines.append("return;")

    def emit_branch(self, insn: Instruction) -> None:
        condition = self.condition(insn)
        target = f"0x{insn.target:06X}u"
        following = f"0x{insn.next_address:06X}u"
        body = []
        if insn.branch_link:
            body.append(f"nsl_cpu.r[14] = nsl_r15({following});")
        if condition is None:
            body.append(f"nsl_cpu.pc = {target};")
        else:
            body.append(f"if ({condition}) {{")
            body.append(f"\tnsl_cpu.pc = {target};")
            body.append("} else {")
            body.append(f"\tnsl_cpu.pc = {following};")
            body.append("}")
        body.append("return;")
        self.lines.extend(body)

    def emit(self) -> list[str]:
        self.line(f"void nsl_b_{self.block.start:06X}(void) {{")
        self.line(f"\tnsl_tick({max(1, len(self.block.instructions))});")
        for insn in self.block.instructions:
            self.address = insn.address
            self.line(f"\t/* {insn.address:06X}: {text(insn)} */")
            if insn.kind == "dp":
                self.emit_dp(insn)
            elif insn.kind == "ldr":
                self.emit_ldr(insn)
            elif insn.kind == "ldm":
                self.emit_ldm(insn)
            elif insn.kind == "branch":
                self.emit_branch(insn)
            elif insn.kind == "mul":
                self.emit_mul(insn)
            elif insn.kind in ("mcr", "mrc", "cdp"):
                self.emit_copro(insn)
            elif insn.kind == "swi":
                self.line(f"\tnsl_cpu_fault(\"SWI %06X\", 0x{insn.swi_number:06X}u);")
                self.line("\treturn;")
            else:
                self.line(f"\tnsl_cpu_note_undefined(0x{insn.address:06X}u, 0x{insn.word:08X}u);")
                self.line("\treturn;")
        # a block that did not end in a PC write falls through to its next address
        last = self.block.instructions[-1]
        if not (last.kind in ("branch",) or last.writes_pc):
            self.line(f"\tnsl_cpu.pc = 0x{last.next_address:06X}u;")
            self.line("\treturn;")
        self.line("}")
        self.line("")
        return self.lines

    def emit_mul(self, insn: Instruction) -> None:
        result = f"m{insn.address & 0xFFFF:x}"
        body = [
            f"uint32_t {result} = nsl_mul({reg(insn.rm)}, {reg(insn.rs)}, "
            f"{reg(insn.rn) if insn.opcode else '0'}, {1 if insn.opcode else 0}, "
            f"{1 if insn.set_flags else 0});",
            f"{reg(insn.rd)} = {result};",
        ]
        self.lines.extend(self.wrap(insn, body))

    def emit_copro(self, insn: Instruction) -> None:
        body: list[str] = []
        if insn.cp_number != 0:
            body.append(f"nsl_cpu_fault(\"coprocessor p{insn.cp_number}\");")
            body.append("return;")
            self.lines.extend(self.wrap(insn, body))
            return
        if insn.kind == "mrc":
            body.append(f"{reg(insn.rd)} = nsl_copro_read({insn.cp_creg});")
        elif insn.kind == "mcr":
            body.append(f"nsl_copro_write({insn.cp_creg}, {reg(insn.rd)});")
        else:
            body.append("nsl_copro_divide();")
        self.lines.extend(self.wrap(insn, body))


@dataclass
class Program:
    image: bytes
    analysis: Analysis
    region: str

    def blocks(self):
        return [self.analysis.blocks[start] for start in sorted(self.analysis.blocks)]


def _header(blocks: list[int]) -> str:
    lines = [
        "/* Generated by tools/recompile.py from the player's ROM. Do not redistribute. */",
        "#ifndef NSL_PROGRAM_BLOCKS_H",
        "#define NSL_PROGRAM_BLOCKS_H",
        "",
        "#include \"nsl/cpu.h\"",
        "#include \"nsl/run.h\"",
        "",
        "#define R0 nsl_cpu.r[0]",
        "#define R1 nsl_cpu.r[1]",
        "#define R2 nsl_cpu.r[2]",
        "#define R3 nsl_cpu.r[3]",
        "#define R4 nsl_cpu.r[4]",
        "#define R5 nsl_cpu.r[5]",
        "#define R6 nsl_cpu.r[6]",
        "#define R7 nsl_cpu.r[7]",
        "#define R8 nsl_cpu.r[8]",
        "#define R9 nsl_cpu.r[9]",
        "#define R10 nsl_cpu.r[10]",
        "#define R11 nsl_cpu.r[11]",
        "#define R12 nsl_cpu.r[12]",
        "#define R13 nsl_cpu.r[13]",
        "#define R14 nsl_cpu.r[14]",
        "",
    ]
    for block in blocks:
        lines.append(f"void nsl_b_{block:06X}(void);")
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def write_program(program: Program, directory: Path, blocks_per_file: int = 1200) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    blocks = program.blocks()
    starts = [block.start for block in blocks]
    (directory / "program_blocks.h").write_text(_header(starts), encoding="utf-8")

    total = 0
    for index, block in enumerate(blocks):
        total += len(block.instructions)
    chunk_count = 0
    for first in range(0, len(blocks), blocks_per_file):
        chunk = blocks[first:first + blocks_per_file]
        chunk_count += 1
        name = f"program_{chunk_count:02d}.c"
        text_out = [
            "/* Generated by tools/recompile.py from the player's ROM. Do not redistribute. */",
            "#include \"program_blocks.h\"",
            "",
        ]
        for block in chunk:
            text_out.extend(BlockWriter(program, block).emit())
        (directory / name).write_text("\n".join(text_out), encoding="utf-8")

    table = [
        "/* Generated by tools/recompile.py from the player's ROM. Do not redistribute. */",
        "#include \"program_blocks.h\"",
        "#include <stdio.h>",
        "",
        "typedef struct NslBlock { uint32_t pc; void (*fn)(void); } NslBlock;",
        "static const NslBlock nsl_blocks[] = {",
    ]
    for start in starts:
        table.append(f"    {{ 0x{start:06X}u, nsl_b_{start:06X} }},")
    table += [
        "};",
        "",
        "void nsl_dispatch(void) {",
        "    uint32_t pc = nsl_cpu.pc & 0x03FFFFFCu;",
        "    int low = 0;",
        "    int high = (int)(sizeof(nsl_blocks) / sizeof(nsl_blocks[0])) - 1;",
        "    while (low <= high) {",
        "        int middle = (low + high) >> 1;",
        "        if (nsl_blocks[middle].pc < pc) {",
        "            low = middle + 1;",
        "        } else if (nsl_blocks[middle].pc > pc) {",
        "            high = middle - 1;",
        "        } else {",
        "            nsl_cpu.last_block = pc;",
        "            nsl_cpu.history[nsl_cpu.history_index++ & 31] = pc;",
        "            if (nsl_cpu.trace_blocks > 0) {",
        "                if (nsl_cpu.trace_after > 0) {",
        "                    nsl_cpu.trace_after--;",
        "                } else {",
        "                    nsl_cpu.trace_blocks--;",
        "                    fprintf(stderr, \"%06X sp=%08X lr=%08X r0=%08X r1=%08X r2=%08X\\n\",",
        "                            pc, nsl_cpu.r[13], nsl_cpu.r[14], nsl_cpu.r[0], nsl_cpu.r[1], nsl_cpu.r[2]);",
        "                }",
        "            }",
        "            if (nsl_cpu.trace_pc == pc && nsl_cpu.trace_pc_count > 0) {",
        "                nsl_cpu.trace_pc_count--;",
        "                fprintf(stderr, \"trace %06X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X lr=%08X sp=%08X z=%u\\n\",",
        "                        pc, nsl_cpu.r[0], nsl_cpu.r[1], nsl_cpu.r[2], nsl_cpu.r[3], nsl_cpu.r[4], nsl_cpu.r[5], nsl_cpu.r[6], nsl_cpu.r[7], nsl_cpu.r[8], nsl_cpu.r[9], nsl_cpu.r[10], nsl_cpu.r[11], nsl_cpu.r[12], nsl_cpu.r[14], nsl_cpu.r[13], nsl_cpu.z);",
        "                if (nsl_cpu.r[1] < 16) {",
        "                    fprintf(stderr, \"  hist:\");",
        "                    for (int h = 0; h < 16; h++) {",
        "                        unsigned at = (nsl_cpu.history_index - 16 + h) & 31;",
        "                        fprintf(stderr, \" %06X\", nsl_cpu.history[at]);",
        "                    }",
        "                    fprintf(stderr, \"\\n\");",
        "                }",
        "            }",
        "            nsl_blocks[middle].fn();",
        "            return;",
        "        }",
        "    }",
        "    nsl_cpu_fault(\"no translated block at %06X (last %06X, lr %08X, sp %08X, mode %u)\",",
        "                   pc, nsl_cpu.last_block, nsl_cpu.r[14], nsl_cpu.r[13], nsl_cpu.mode);",
        "    fprintf(stderr, \"history:\");",
        "    for (int i = 0; i < 16; i++) {",
        "        unsigned at = (nsl_cpu.history_index - 16 + i) & 31;",
        "        fprintf(stderr, \" %06X\", nsl_cpu.history[at]);",
        "    }",
        "    fprintf(stderr, \"\\n\");",
        "}",
        "",
    ]
    (directory / "program_table.c").write_text("\n".join(table), encoding="utf-8")
    (directory / "program.h").write_text(
        "/* Generated by tools/recompile.py. */\n"
        f"#define NSL_PROGRAM_REGION \"{program.region}\"\n"
        f"#define NSL_PROGRAM_BLOCKS {len(blocks)}\n"
        f"#define NSL_PROGRAM_INSTRUCTIONS {total}\n",
        encoding="utf-8",
    )
    return {
        "blocks": len(blocks),
        "instructions": total,
        "files": chunk_count + 2,
        "bytes": total * 4,
        "coverage": total * 4 / len(program.image),
        "indirect": len(program.analysis.indirect_pcs),
        "literals": len(program.analysis.literals),
        "functions": len(program.analysis.functions),
    }
