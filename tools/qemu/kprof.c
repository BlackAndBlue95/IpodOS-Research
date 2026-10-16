/* kprof: QEMU TCG plugin, instruction and memory profile of the iPod OS with an ARM926-like L1 model.
 * Build line and arguments: tools/qemu/README.md; analyse with kprof.py. */
#include <glib.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct { uint64_t va; uint32_t n, bytes; uint64_t cnt, irq, imiss, dmiss, wmiss; } TB;
typedef struct { uint32_t pc; TB *t; } Ins;

static FILE *out;
static GHashTable *tbs;
static uint64_t trig[32]; static int ntrig, trig_hit[32];
static uint64_t lrpc[8]; static int nlrpc;
static struct { uint32_t pc, lr, a; uint64_t n[6]; } lrh[8192]; static int nlrh;
static GByteArray *rbuf;
static int irq_depth, iramc;
typedef struct { uint32_t pc, lr, size, dst, src; uint64_t n[6], ins[6], dm[6], wm[6]; } Cp;
static uint64_t cppc[8]; static int cpreg[8], ncp, cp_ph;
static GHashTable *cps; static Cp *cur_cp; static uint32_t cur_ret;
static char markf[512]; static uint64_t mark_poll; static int nmark;
static uint64_t wrlo[4], wrhi[4]; static int nwr; static GHashTable *wrh;

enum { R_IRAM, R_DRAM, R_DRAMU, R_IO, R_E, R_OTHER, NR };
static const char *rname[NR] = { "iram", "dram", "dram-uncached", "io", "regionE", "other" };
static uint64_t ins_tot, ins_idle, ins_irq, ins_sched, ins_by[NR], rd_by[NR], wr_by[NR];
#define SETS 128
#define WAYS 4
typedef struct { uint32_t tag[SETS][WAYS]; uint8_t valid[SETS][WAYS], dirty[SETS][WAYS], rr[SETS]; } Cache;
static Cache ic, dc;
static uint64_t i_acc, i_miss, i_unc, d_rd, d_rdmiss, d_wr, d_wrmiss, d_wb;
static uint64_t cw_n; static uint32_t cw_pc[16], cw_addr[16]; static int ncw;   /* stores into IRAM code pages */

static uint32_t reg(int r) { g_byte_array_set_size(rbuf, 0); return qemu_plugin_read_register(rbuf, r) == 4 ? *(uint32_t *)rbuf->data : 0; }
static int phase_now(void) { int p = nmark; for (int i = 0; i < ntrig; i++) p += trig_hit[i]; return p > 5 ? 5 : p; }
static int in_libc(uint64_t va) { if (va < 0x100000) va += 0x22000000; return va >= 0x22000020 && va < 0x22000318; }

static int region(uint64_t a)
{
    if (a < 0x100000 || (a >= 0x22000000 && a < 0x22100000)) return R_IRAM;
    if (a >= 0x08b33000 && a < 0x08e00000) return R_E;
    if (a >= 0x08000000 && a < 0x0c000000) return R_DRAM;
    if (a >= 0x88000000 && a < 0x8c000000) return R_DRAMU;
    if (a >= 0x38000000 && a < 0x40000000) return R_IO;
    return R_OTHER;
}
/* Apple's page table: DRAM write-back, IRAM, IO and the 0x88000000 DRAM alias uncached */
static int cached(uint64_t va, int r)
{
    return r == R_DRAM || r == R_E || (iramc && ((va >= 0x22000000 && va < 0x22008000) || va < 0x8000));
}

static int lookup(Cache *c, uint32_t a, int alloc, int wr)
{
    uint32_t line = a >> 5, set = line & (SETS - 1), tag = line >> 7;
    for (int w = 0; w < WAYS; w++)
        if (c->valid[set][w] && c->tag[set][w] == tag) { if (wr) c->dirty[set][w] = 1; return 1; }
    if (alloc) {
        int w = c->rr[set]++ & (WAYS - 1);
        if (c->valid[set][w] && c->dirty[set][w]) d_wb++;
        c->valid[set][w] = 1; c->tag[set][w] = tag; c->dirty[set][w] = 0;
    }
    return 0;
}

static void snapshot(const char *label, uint64_t pc)
{
    fprintf(out, "SNAP %s pc=%08" PRIx64 " ins=%" PRIu64 " idle=%" PRIu64 " irq=%" PRIu64 " sched=%" PRIu64,
            label, pc, ins_tot, ins_idle, ins_irq, ins_sched);
    for (int r = 0; r < NR; r++) fprintf(out, " ins_%s=%" PRIu64, rname[r], ins_by[r]);
    for (int r = 0; r < NR; r++) fprintf(out, " rd_%s=%" PRIu64 " wr_%s=%" PRIu64, rname[r], rd_by[r], rname[r], wr_by[r]);
    fprintf(out, " i_acc=%" PRIu64 " i_miss=%" PRIu64 " i_unc=%" PRIu64 " d_rd=%" PRIu64 " d_rdmiss=%" PRIu64
            " d_wr=%" PRIu64 " d_wrmiss=%" PRIu64 " d_wb=%" PRIu64 "\n", i_acc, i_miss, i_unc, d_rd, d_rdmiss, d_wr, d_wrmiss, d_wb);
}

static void dump(const char *label)
{
    GHashTableIter it; gpointer k, v;
    fprintf(out, "TBDUMP %s\n", label);
    g_hash_table_iter_init(&it, tbs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        TB *t = v;
        if (t->cnt) fprintf(out, "TB %08" PRIx64 " %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                            t->va, t->n, t->cnt, t->irq, t->imiss, t->dmiss, t->wmiss);
    }
    fprintf(out, "CODEWRITES %" PRIu64, cw_n);
    for (int i = 0; i < ncw; i++) fprintf(out, " %08x@%08x", cw_addr[i], cw_pc[i]);
    fprintf(out, "\n");
    for (int i = 0; i < nlrh; i++)
        fprintf(out, "LR %08x %08x %08x %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", lrh[i].pc, lrh[i].lr, lrh[i].a,
                lrh[i].n[0], lrh[i].n[1], lrh[i].n[2], lrh[i].n[3], lrh[i].n[4], lrh[i].n[5]);
    g_hash_table_iter_init(&it, wrh);   /* WR range pc phase stores */
    while (g_hash_table_iter_next(&it, &k, &v))
        fprintf(out, "WR %d %08x %d %" PRIu64 "\n", (int)(*(uint64_t *)k >> 32 & 0xff), (uint32_t)*(uint64_t *)k,
                (int)(*(uint64_t *)k >> 40), *(uint64_t *)v);
    if (cps) {   /* CP pc lr size dst src phase calls ins dmiss wmiss, cumulative */
        g_hash_table_iter_init(&it, cps);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            Cp *c = v;
            for (int p = 0; p < 6; p++)
                if (c->n[p]) fprintf(out, "CP %08x %08x %u %08x %08x %d %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                                     c->pc, c->lr, c->size, c->dst, c->src, p, c->n[p], c->ins[p], c->dm[p], c->wm[p]);
        }
    }
    fflush(out);
}

static void tb_exec(unsigned int cpu, void *ud)
{
    TB *t = ud;
    int r = region(t->va);
    uint64_t lo = t->va < 0x100000 ? t->va + 0x22000000 : t->va;   /* the IRQ path runs at the VA 0 alias */
    if (!(++mark_poll & 0xffff) &&!access(markf, F_OK)) {
        char l[8]; unlink(markf); snprintf(l, sizeof l, "m%d", nmark); snapshot(l, t->va); dump(l); nmark++;
    }
    if (cur_cp && (uint32_t)t->va == cur_ret) cur_cp = NULL;
    for (int i = 0; i < ncp; i++)
        if (t->va == cppc[i] && !(cur_cp && reg(14) == cur_ret)) {   /* an internal branch to another entry is the same call */
            uint32_t lr = reg(14), size = reg(cpreg[i]);
            uint64_t key = (uint64_t)i << 61 | (uint64_t)(size & 0x1fffffff) << 32 | lr;
            Cp *c = g_hash_table_lookup(cps, &key);
            if (!c) {
                uint64_t *kp = g_new(uint64_t, 1); *kp = key;
                c = g_new0(Cp, 1); c->pc = (uint32_t)t->va; c->lr = lr; c->size = size; c->dst = reg(0); c->src = reg(1);
                g_hash_table_insert(cps, kp, c);
            }
            cp_ph = phase_now(); c->n[cp_ph]++; cur_cp = c; cur_ret = lr;
            break;
        }
    if (cur_cp && irq_depth == 0 && in_libc(t->va)) cur_cp->ins[cp_ph] += t->n;
    t->cnt++;
    ins_tot += t->n; ins_by[r] += t->n;
    if (lo >= 0x22003b3c && lo < 0x22003b64) ins_idle += t->n;
    if (lo == 0x22003080) irq_depth++;
    if (irq_depth > 0) { ins_irq += t->n; t->irq++; }
    if (lo == 0x220030d8 && irq_depth > 0) irq_depth--;
    if (lo >= 0x220034e8 && lo < 0x22003600) ins_sched += t->n;
    if (cached(t->va, r)) {
        for (uint32_t a = t->va & ~31u; a < t->va + t->bytes; a += 32) { i_acc++; if (!lookup(&ic, a, 1, 0)) { i_miss++; t->imiss++; } }
    } else i_unc += t->n;
    for (int i = 0; i < nlrpc; i++)
        if (t->va == lrpc[i]) {   /* registers are exact only at a TB head */
            uint32_t lr = reg(14), a = reg(0) ^ (reg(1) << 16), pc = (uint32_t)t->va;
            int j;
            for (j = 0; j < nlrh && !(lrh[j].pc == pc && lrh[j].lr == lr && lrh[j].a == a); j++) ;
            if (j == nlrh && nlrh < 8192) { lrh[j].pc = pc; lrh[j].lr = lr; lrh[j].a = a; nlrh++; }
            if (j < nlrh) lrh[j].n[phase_now()]++;
        }
    for (int i = 0; i < ntrig; i++)
        if (!trig_hit[i] && t->va == trig[i]) { char l[8]; trig_hit[i] = 1; snprintf(l, sizeof l, "t%d", i); snapshot(l, t->va); dump(l); }
}

static void mem_cb(unsigned int cpu, qemu_plugin_meminfo_t info, uint64_t va, void *ud)
{
    Ins *in = ud;
    int r = region(va), st = qemu_plugin_mem_is_store(info);
    if (st) wr_by[r]++; else rd_by[r]++;
    if (st && (va < 0x8000 || (va >= 0x22000000 && va < 0x22008000)) && !(in->pc >= 0x08000000 && in->pc < 0x08000400)) {
        cw_n++;                     /* not the startup copy */
        if (ncw < 16) { cw_pc[ncw] = in->pc; cw_addr[ncw] = (uint32_t)va; ncw++; }
    }
    for (int i = 0; st && i < nwr; i++)
        if (va >= wrlo[i] && va < wrhi[i]) {
            uint64_t key = (uint64_t)phase_now() << 40 | (uint64_t)i << 32 | in->pc, *n = g_hash_table_lookup(wrh, &key);
            if (!n) { uint64_t *kp = g_new(uint64_t, 1); *kp = key; n = g_new0(uint64_t, 1); g_hash_table_insert(wrh, kp, n); }
            (*n)++;
        }
    if (!cached(va, r)) return;
    int cp = cur_cp && irq_depth == 0 && in_libc(in->pc);
    if (st) { d_wr++; if (!lookup(&dc, (uint32_t)va, 0, 1)) { d_wrmiss++; in->t->wmiss++; if (cp) cur_cp->wm[cp_ph]++; } }
    else { d_rd++; if (!lookup(&dc, (uint32_t)va, 1, 0)) { d_rdmiss++; in->t->dmiss++; if (cp) cur_cp->dm[cp_ph]++; } }
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    uint64_t va = qemu_plugin_tb_vaddr(tb);
    size_t n = qemu_plugin_tb_n_insns(tb);
    TB *t = g_hash_table_lookup(tbs, &va);
    if (!t) {
        t = g_new0(TB, 1); t->va = va; t->n = n;
        t->bytes = n ? (uint32_t)(qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, n - 1)) + 4 - va) : 0;
        g_hash_table_insert(tbs, &t->va, t);
    }
    qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS, t);
    for (size_t i = 0; i < n; i++) {
        Ins *in = g_new0(Ins, 1);
        in->pc = (uint32_t)qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, i)); in->t = t;
        qemu_plugin_register_vcpu_mem_cb(qemu_plugin_tb_get_insn(tb, i), mem_cb, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, in);
    }
}

static void at_exit(qemu_plugin_id_t id, void *p) { if (!out) return; snapshot("exit", 0); dump("exit"); fclose(out); out = NULL; }

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    const char *o = "kprof.out";
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) o = argv[i] + 4;
        else if (!strncmp(argv[i], "trig=", 5) && ntrig < 32) trig[ntrig++] = strtoull(argv[i] + 5, NULL, 0);
        else if (!strncmp(argv[i], "lr=", 3) && nlrpc < 8) lrpc[nlrpc++] = strtoull(argv[i] + 3, NULL, 0);
        else if (!strncmp(argv[i], "iramc=", 6)) iramc = atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "wr=", 3) && nwr < 4) {
            char *e; wrlo[nwr] = strtoull(argv[i] + 3, &e, 0); wrhi[nwr++] = strtoull(e + 1, NULL, 0);
        }
        else if (!strncmp(argv[i], "cp=", 3) && ncp < 8) {
            char *e; cppc[ncp] = strtoull(argv[i] + 3, &e, 0); cpreg[ncp++] = *e == ':' ? atoi(e + 1) : 2;
        }
    }
    if (ncp) cps = g_hash_table_new(g_int64_hash, g_int64_equal);
    wrh = g_hash_table_new(g_int64_hash, g_int64_equal);
    snprintf(markf, sizeof markf, "%s.mark", o);
    out = fopen(o, "w");
    rbuf = g_byte_array_new();
    tbs = g_hash_table_new(g_int64_hash, g_int64_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
