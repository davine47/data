/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "riscv_branch_decode.h"

static unsigned int checks;

static void check(uint32_t word, size_t size, unsigned int xlen,
                  BranchKind expected)
{
    uint8_t bytes[4] = {
        word & 255, (word >> 8) & 255,
        (word >> 16) & 255, (word >> 24) & 255,
    };
    BranchKind actual = riscv_branch_kind(bytes, size, xlen);
    checks++;
    if (actual != expected) {
        fprintf(stderr,
            "FAIL word=%08x size=%zu xlen=%u expected=%d actual=%d\n",
            word, size, xlen, expected, actual);
        exit(EXIT_FAILURE);
    }
}

int main(void)
{
    for (unsigned int xlen = 32; xlen <= 64; xlen += 32) {
        /* All B-type funct3 values, including the two reserved values. */
        for (unsigned int f = 0; f < 8; f++) {
            check(0x63 | (f << 12), 4, xlen,
                  (f == 2 || f == 3) ? BRANCH_OTHER : BRANCH_CONDITIONAL);
            check(0x67 | (f << 12), 4, xlen,
                  f == 0 ? BRANCH_INDIRECT : BRANCH_OTHER);
        }
        check(0x0000006f, 4, xlen, BRANCH_DIRECT);   /* JAL x0 */
        check(0x000000ef, 4, xlen, BRANCH_DIRECT);   /* JAL ra */
        check(0x00008067, 4, xlen, BRANCH_INDIRECT); /* RET */
        check(0x00000073, 4, xlen, BRANCH_OTHER);    /* ECALL */
        check(0x00100073, 4, xlen, BRANCH_OTHER);    /* EBREAK */
        check(0x10200073, 4, xlen, BRANCH_OTHER);    /* SRET */
        check(0x30200073, 4, xlen, BRANCH_OTHER);    /* MRET */
        check(0x00000013, 4, xlen, BRANCH_OTHER);    /* NOP */
        check(0x0000a001, 2, xlen, BRANCH_DIRECT);   /* C.J */
        check(0x0000c001, 2, xlen, BRANCH_CONDITIONAL);
        check(0x0000e001, 2, xlen, BRANCH_CONDITIONAL);
        check(0x00008082, 2, xlen, BRANCH_INDIRECT); /* C.JR ra */
        check(0x00009082, 2, xlen, BRANCH_INDIRECT); /* C.JALR ra */
        check(0x00009002, 2, xlen, BRANCH_OTHER);    /* C.EBREAK */
        check(0x00008002, 2, xlen, BRANCH_OTHER);    /* reserved C.JR x0 */
        check(0x00008086, 2, xlen, BRANCH_OTHER);    /* C.MV */
        check(0x00009086, 2, xlen, BRANCH_OTHER);    /* C.ADD */
        check(0x00002081, 2, xlen,
              xlen == 32 ? BRANCH_DIRECT : BRANCH_OTHER);

        /* Exhaustively verify the 16-bit base I+C classification space. */
        for (uint32_t word = 0; word < 65536; word++) {
            BranchKind expected = BRANCH_OTHER;
            if ((word & 0xe003) == 0xc001 ||
                (word & 0xe003) == 0xe001) {
                expected = BRANCH_CONDITIONAL;
            } else if ((word & 0xe003) == 0xa001 ||
                       (xlen == 32 && (word & 0xe003) == 0x2001)) {
                expected = BRANCH_DIRECT;
            } else if (((word & 0xf07f) == 0x8002 ||
                        (word & 0xf07f) == 0x9002) && (word & 0x0f80)) {
                expected = BRANCH_INDIRECT;
            }
            check(word, 2, xlen, expected);
        }
    }
    check(0, 0, 64, BRANCH_OTHER);
    check(0, 3, 64, BRANCH_OTHER);
    check(0x6f, 4, 128, BRANCH_OTHER);
    printf("PASS: %u decoder checks\n", checks);
    return EXIT_SUCCESS;
}
