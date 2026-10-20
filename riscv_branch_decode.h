/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RISCV_BRANCH_DECODE_H
#define RISCV_BRANCH_DECODE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BRANCH_OTHER,
    BRANCH_CONDITIONAL,
    BRANCH_DIRECT,
    BRANCH_INDIRECT,
} BranchKind;

/* Standard RV32/RV64 I+C control transfers only; not an ISA validator.
 * XLEN must match the execution mode. Instruction parcels are little-endian.
 */
static inline BranchKind riscv_branch_kind(const uint8_t *bytes,
                                            size_t size, unsigned int xlen)
{
    if (xlen != 32 && xlen != 64) {
        return BRANCH_OTHER;
    }

    if (size == 4) {
        uint32_t word = (uint32_t)bytes[0] |
                        ((uint32_t)bytes[1] << 8) |
                        ((uint32_t)bytes[2] << 16) |
                        ((uint32_t)bytes[3] << 24);
        unsigned int opcode = word & 0x7f;
        unsigned int funct3 = (word >> 12) & 7;

        if (opcode == 0x63 &&
            (funct3 == 0 || funct3 == 1 || funct3 >= 4)) {
            return BRANCH_CONDITIONAL;
        }
        if (opcode == 0x6f) {
            return BRANCH_DIRECT;
        }
        if (opcode == 0x67 && funct3 == 0) {
            return BRANCH_INDIRECT;
        }
    } else if (size == 2) {
        uint16_t word = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
        unsigned int quadrant = word & 3;
        unsigned int funct3 = (word >> 13) & 7;

        if (quadrant == 1) {
            if (funct3 == 6 || funct3 == 7) { /* C.BEQZ, C.BNEZ */
                return BRANCH_CONDITIONAL;
            }
            if (funct3 == 5 || (xlen == 32 && funct3 == 1)) {
                return BRANCH_DIRECT;       /* C.J, RV32 C.JAL */
            }
        }

        if (quadrant == 2 && funct3 == 4 &&
            ((word >> 2) & 31) == 0 && ((word >> 7) & 31) != 0) {
            return BRANCH_INDIRECT;         /* C.JR, C.JALR; not C.EBREAK */
        }
    }

    return BRANCH_OTHER;
}

#endif
