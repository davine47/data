/* SPDX-License-Identifier: GPL-2.0-or-later
 * Local research/reference plugin: dynamic RISC-V I+C branch mix.
 * Verified against the local QEMU 11.1.50 plugin API (version 7).
 */
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

#include "riscv_branch_decode.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    uint64_t instructions;
    uint64_t conditional;
    uint64_t direct;
    uint64_t indirect;
    uint64_t unsupported_length;
    uint64_t decode_errors;
} BranchStats;

static struct qemu_plugin_scoreboard *stats;
static unsigned int guest_xlen;
static bool system_mode;
static char *target_name;
static FILE *report_file;

#define COUNTER(member) \
    qemu_plugin_scoreboard_u64_in_struct(stats, BranchStats, member)

static void add_counter(struct qemu_plugin_insn *insn, qemu_plugin_u64 entry)
{
    qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
        insn, QEMU_PLUGIN_INLINE_ADD_U64, entry, 1);
}

static void on_tb_translate(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;

    for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        size_t size = qemu_plugin_insn_size(insn);
        uint8_t bytes[4];

        add_counter(insn, COUNTER(instructions));

        if (size != 2 && size != 4) {
            add_counter(insn, COUNTER(unsupported_length));
            continue;
        }
        if (qemu_plugin_insn_data(insn, bytes, size) != size) {
            add_counter(insn, COUNTER(decode_errors));
            continue;
        }

        switch (riscv_branch_kind(bytes, size, guest_xlen)) {
        case BRANCH_CONDITIONAL:
            add_counter(insn, COUNTER(conditional));
            break;
        case BRANCH_DIRECT:
            add_counter(insn, COUNTER(direct));
            break;
        case BRANCH_INDIRECT:
            add_counter(insn, COUNTER(indirect));
            break;
        case BRANCH_OTHER:
            break;
        }
    }
}

static void append_report(GString *out, const char *label,
                          const BranchStats *s)
{
    uint64_t branches = s->conditional + s->direct + s->indirect;

    g_string_append_printf(out,
        "%s\n"
        "  instructions = %" PRIu64 "\n"
        "  conditional = %" PRIu64 "\n"
        "  direct = %" PRIu64 "\n"
        "  indirect = %" PRIu64 "\n"
        "  branches = %" PRIu64 "\n"
        "  unsupported_length = %" PRIu64 "\n"
        "  decode_errors = %" PRIu64 "\n",
        label, s->instructions, s->conditional, s->direct, s->indirect,
        branches, s->unsupported_length, s->decode_errors);

    if (branches) {
        g_string_append_printf(out,
            "  conditional_ratio = %.9f%%\n",
            100.0 * (double)s->conditional / (double)branches);
    } else {
        g_string_append(out, "  conditional_ratio = N/A (no branches)\n");
    }
    g_string_append_c(out, '\n');
}

static void plugin_exit(void *userdata)
{
    (void)userdata;
    GString *out = g_string_new("RISC-V dynamic branch statistics\n");

    g_string_append_printf(out,
        "target = %s\n"
        "xlen = %u\n"
        "mode = %s\n"
        "scope = all instrumented guest instructions\n"
        "coverage = standard I+C branch/jump encodings\n"
        "counting = pre-execution events, not guaranteed retired instructions\n"
        "calls_and_returns = included as JAL/JALR forms\n\n",
        target_name, guest_xlen, system_mode ? "system" : "user");

    for (int i = 0; i < qemu_plugin_num_vcpus(); i++) {
        const BranchStats *s = qemu_plugin_scoreboard_find(stats, i);
        char label[48];
        snprintf(label, sizeof(label), "vcpu %d:", i);
        append_report(out, label, s);
    }

    BranchStats total = {
        .instructions = qemu_plugin_u64_sum(COUNTER(instructions)),
        .conditional = qemu_plugin_u64_sum(COUNTER(conditional)),
        .direct = qemu_plugin_u64_sum(COUNTER(direct)),
        .indirect = qemu_plugin_u64_sum(COUNTER(indirect)),
        .unsupported_length = qemu_plugin_u64_sum(COUNTER(unsupported_length)),
        .decode_errors = qemu_plugin_u64_sum(COUNTER(decode_errors)),
    };
    append_report(out, "total:", &total);

    if (report_file) {
        bool write_failed = fwrite(out->str, 1, out->len, report_file) != out->len;
        if (fclose(report_file) != 0) {
            write_failed = true;
        }
        if (write_failed) {
            fprintf(stderr, "branchstat: failed to write report\n");
        }
        report_file = NULL;
    } else {
        /* Enable with QEMU's -d plugin; optionally redirect using -D FILE. */
        qemu_plugin_outs(out->str);
    }

    g_string_free(out, true);
    qemu_plugin_scoreboard_free(stats);
    g_free(target_name);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                          const qemu_info_t *info,
                                          int argc, char **argv)
{
    const char *outfile = NULL;

    if (strcmp(info->target_name, "riscv32") == 0) {
        guest_xlen = 32;
    } else if (strcmp(info->target_name, "riscv64") == 0) {
        guest_xlen = 64;
    } else {
        fprintf(stderr, "branchstat: requires riscv32 or riscv64, got %s\n",
                info->target_name);
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        if (!g_str_has_prefix(argv[i], "outfile=") || !argv[i][8] || outfile) {
            fprintf(stderr,
                "branchstat: expected at most one outfile=PATH argument, got %s\n",
                argv[i]);
            return -1;
        }
        outfile = argv[i] + 8;
    }

    if (outfile && strcmp(outfile, "-") != 0) {
        /* Exclusive creation prevents accidental loss of a previous report. */
        report_file = fopen(outfile, "wx");
        if (!report_file) {
            fprintf(stderr, "branchstat: cannot create %s: %s\n",
                    outfile, strerror(errno));
            return -1;
        }
    }

    target_name = g_strdup(info->target_name);
    system_mode = info->system_emulation;
    stats = qemu_plugin_scoreboard_new(sizeof(BranchStats));
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_translate, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
