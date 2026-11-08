"""Static analysis of the DE156 ARM2 program: code discovery without traces.

The program's exception vectors at 0x00-0x1C are the roots. Direct branches and calls add their
targets; a conditional branch also adds its fall-through. A PC-relative load establishes a
literal pool (the words are data, not code); a load into PC from such a literal is a computed
jump whose target is the literal's value. Anything else that writes PC is recorded so the
translator can fall back to the runtime dispatcher.

This is deliberately conservative: it never guesses that a random data word is code. Code that
only a table reaches is added by the explicit table rules below or by the trace-assisted
development mode.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .decode import DecodeError, Instruction, decode


@dataclass
class Block:
    start: int
    instructions: list[Instruction] = field(default_factory=list)
    successors: list[int] = field(default_factory=list)
    indirect: bool = False
    reason: str = ""


def constant_pc_target(instruction: Instruction) -> int | None:
    """Where a data-processing instruction that writes PC goes, when its operands are constants."""
    if instruction.kind != "dp" or instruction.rd != 15 or instruction.opcode in (0x8, 0x9, 0xA, 0xB):
        return None
    operand = instruction.operand2
    if not operand.immediate:
        return None
    pc = instruction.pc_value
    opcode = instruction.opcode
    rn = instruction.rn
    base = pc if rn == 15 else None
    value = operand.value
    if opcode == 0xD:                      # MOV
        result = value
    elif opcode == 0xF:                    # MVN
        result = (~value) & 0xFFFFFFFF
    elif base is None:
        return None
    elif opcode == 0x0:                    # AND
        result = base & value
    elif opcode == 0x1:                    # EOR
        result = base ^ value
    elif opcode == 0x2:                    # SUB
        result = (base - value) & 0xFFFFFFFF
    elif opcode == 0x3:                    # RSB
        result = (value - base) & 0xFFFFFFFF
    elif opcode == 0x4:                    # ADD
        result = (base + value) & 0xFFFFFFFF
    elif opcode == 0xC:                    # ORR
        result = base | value
    elif opcode == 0xE:                    # BIC
        result = base & (~value & 0xFFFFFFFF)
    else:
        return None
    return result & 0x03FFFFFC


def _shift_constant(value: int, kind: int, amount: int, carry: int = 0) -> int | None:
    if kind == 0:
        return (value << amount) & 0xFFFFFFFF if amount else value
    if kind == 1:
        return (value >> amount) if amount else 0
    if kind == 2:
        signed = value if value < 0x80000000 else value - 0x100000000
        return (signed >> amount) & 0xFFFFFFFF if amount else (0 if signed < 0 else 0)
    if amount == 0:
        return ((carry << 31) | (value >> 1)) & 0xFFFFFFFF
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF


def evaluate_constant(instruction: Instruction, known: dict[int, int]) -> int | None:
    if instruction.kind == "dp":
        if instruction.rd != 15 or instruction.opcode in (0x8, 0x9, 0xA, 0xB):
            return None
        operand = instruction.operand2
        if operand.immediate:
            value = operand.value
        elif operand.rm in known:
            value = known[operand.rm]
            if operand.shift_immediate:
                value = _shift_constant(value, operand.shift, operand.shift_amount)
            else:
                return None
        else:
            return None
        opcode = instruction.opcode
        if opcode == 0xD:
            result = value
        elif opcode == 0xF:
            result = (~value) & 0xFFFFFFFF
        else:
            base = known.get(instruction.rn)
            if base is None:
                return None
            if opcode == 0x0:
                result = base & value
            elif opcode == 0x1:
                result = base ^ value
            elif opcode == 0x2:
                result = (base - value) & 0xFFFFFFFF
            elif opcode == 0x3:
                result = (value - base) & 0xFFFFFFFF
            elif opcode == 0x4:
                result = (base + value) & 0xFFFFFFFF
            elif opcode == 0xC:
                result = base | value
            elif opcode == 0xE:
                result = base & (~value & 0xFFFFFFFF)
            else:
                return None
        return result & 0x03FFFFFC
    if instruction.kind == "ldr" and instruction.load and instruction.rd == 15 and instruction.rn == 15:
        return None
    return None


@dataclass
class Analysis:
    image: bytes
    code: set[int] = field(default_factory=set)
    functions: set[int] = field(default_factory=set)
    blocks: dict[int, Block] = field(default_factory=dict)
    literals: set[int] = field(default_factory=set)
    indirect_pcs: set[int] = field(default_factory=set)
    undefined: dict[int, Instruction] = field(default_factory=dict)
    vector_targets: dict[int, int] = field(default_factory=dict)

    def word(self, address: int) -> int:
        address &= 0xFFFFF
        return int.from_bytes(self.image[address:address + 4], "little")

    def instruction(self, address: int) -> Instruction:
        address &= 0x3FFFFFF
        return decode(self.word(address), address)

    def run(self, seeds: tuple[int, ...] | list[int] = ()) -> "Analysis":
        pending: list[tuple[int, str]] = []
        # entry points recorded at run time by the fallback (tools/traced_targets.txt)
        for seed in seeds:
            pending.append((seed & 0x3FFFFFC, "seed"))
        for vector in range(8):
            handler = self.word(vector * 4)
            # The vector itself is an instruction (normally B); follow its target.
            pending.append((vector * 4, "vector"))
        # Reset also starts at 0, which is the first vector.
        while pending:
            start, reason = pending.pop()
            start &= 0x3FFFFFF
            if start in self.blocks or start in self.literals:
                continue
            block = self.walk(start, reason)
            self.blocks[start] = block
            for successor in block.successors:
                if successor not in self.blocks:
                    pending.append((successor, "flow"))
        # every branch target is a function entry if a call reaches it
        for block in self.blocks.values():
            for instruction in block.instructions:
                if instruction.kind == "branch" and instruction.branch_link:
                    self.functions.add(instruction.target)
        for target in self.vector_targets.values():
            self.functions.add(target)
        return self

    def walk(self, start: int, reason: str) -> Block:
        block = Block(start=start, reason=reason)
        address = start
        known: dict[int, int] = {}
        while True:
            if address in self.literals or address in self.blocks:
                block.indirect = True
                block.successors.append(address)
                break
            instruction = self.instruction(address)
            if instruction.kind == "undefined" or instruction.undefined:
                self.undefined[address] = instruction
                block.indirect = True
                break
            self.code.add(address)
            block.instructions.append(instruction)
            known[15] = instruction.pc_value
            if address < 0x20:
                # exception vector: a branch, but also record the target
                self.vector_targets[address // 4] = instruction.target
            if instruction.kind == "branch":
                if instruction.branch_link:
                    self.functions.add(instruction.target)
                    block.successors.append(instruction.target)
                    block.successors.append(instruction.next_address)
                else:
                    block.successors.append(instruction.target)
                    if instruction.cond != 0xE:
                        block.successors.append(instruction.next_address)
                break
            literal = None
            if instruction.kind == "ldr" and instruction.rn == 15:
                literal = (instruction.pc_value + (instruction.offset if instruction.up else -instruction.offset)) & 0x3FFFFFF
                if 0 <= literal < len(self.image):
                    self.literals.add(literal & ~3)
                    if instruction.load and instruction.rd == 15:
                        target = self.word(literal & ~3)
                        if 0 <= target < len(self.image):
                            block.successors.append(target & ~3)
                            break
                    if instruction.load:
                        known[instruction.rd] = self.word(literal & ~3)
                    else:
                        known.pop(instruction.rd, None)
            elif instruction.kind == "dp":
                value = evaluate_constant(instruction, known)
                if instruction.rd == 15:
                    constant = value
                    if constant is not None and 0 <= constant < len(self.image):
                        block.successors.append(constant & ~3)
                    else:
                        self.indirect_pcs.add(address)
                        block.indirect = True
                    operand = instruction.operand2
                    return_like = (
                        operand is not None and not operand.immediate and operand.rm == 14
                        or instruction.rn == 14
                    )
                    if not return_like:
                        block.successors.append(instruction.next_address)
                    if instruction.cond != 0xE:
                        block.successors.append(instruction.next_address)
                    break
                if value is not None:
                    known[instruction.rd] = value
                else:
                    known.pop(instruction.rd, None)
            elif instruction.kind == "mul":
                known.pop(instruction.rd, None)
            elif instruction.kind in ("ldr", "ldm"):
                if instruction.kind == "ldm":
                    for register in range(16):
                        if instruction.reg_list & (1 << register):
                            known.pop(register, None)
                else:
                    known.pop(instruction.rd, None)
            elif instruction.kind in ("mrc", "swi", "cdp"):
                known.pop(instruction.rd, None)
            if instruction.writes_pc:
                constant = evaluate_constant(instruction, known)
                if constant is not None and 0 <= constant < len(self.image):
                    block.successors.append(constant & ~3)
                else:
                    self.indirect_pcs.add(address)
                    block.indirect = True
                if instruction.cond != 0xE:
                    block.successors.append(instruction.next_address)
                break
            if instruction.kind == "swi":
                # SWI vectors to 0x08 (an infinite loop in this program); do not fall through
                block.successors.append(0x08)
                break
            address = instruction.next_address
        block.successors = list(dict.fromkeys(block.successors))
        return block

    def summary(self) -> dict:
        return {
            "instructions": len(self.code),
            "bytes": len(self.code) * 4,
            "blocks": len(self.blocks),
            "functions": len(self.functions),
            "literals": len(self.literals),
            "indirect_pc_sites": len(self.indirect_pcs),
            "undefined_entries": len(self.undefined),
            "coverage": len(self.code) * 4 / len(self.image),
        }
