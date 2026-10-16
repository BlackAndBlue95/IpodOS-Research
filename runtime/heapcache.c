/* --heap-cache: freed tag-2 blocks up to 512 bytes kept per size for the next os_malloc of that size;
 * Apple's heap still counts them allocated (docs/CODEBASE.md, "Boot CPU"). */
#include "e.h"

#define HC_MAXCAP  512
#define HC_PER     24
#define HC_BYTES   (96u << 10)
#define HC_BINS    (HC_MAXCAP / 8 + 1)
static void *hc_bin[HC_BINS][HC_PER];
static uint8_t hc_n[HC_BINS];
static uint32_t hc_bytes;
static uint32_t hc_hits, hc_miss, hc_kept, hc_flushes, hc_moved;

static inline uint32_t hc_off(void) { uint32_t c, t; __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0xc0\n msr cpsr_c, %1" : "=r"(c), "=r"(t)); return c; }
static inline void hc_on(uint32_t c) { __asm__ volatile("msr cpsr_c, %0" :: "r"(c)); }
static inline uint32_t hc_hdr(void *p) { return *(uint32_t *)((uint8_t *)p - 8); }

#define os_heap        (*(void **)0x089ca638)
#define os_heap_lock   ((void (*)(void *))0x081927f4)
#define os_heap_unlock ((void (*)(void *))0x08191f0c)
#define os_recharge    ((void (*)(void *, void *, uint32_t, uint32_t))0x08191e84)   /* (heap, hdr, size, tag): to the running task */
#define os_task_id     ((uint32_t (*)(void))0x0802cf88)
#define os_alloc       ((void *(*)(void *, uint32_t, uint32_t, uint32_t, void *, uint32_t, uint32_t))0x08192170)

/* Apple's os_free past the hooked first instruction */
__attribute__((naked, used)) static void hc_os_free(void *p, uint32_t tag)
{ __asm__ volatile(".word 0xe92d4070\n ldr pc, 1f\n 1: .word 0x080dca9c"); }

/* hands every cached block back to Apple's heap */
static void hc_flush(void)
{
    uint32_t k, c;
    void *p;
    hc_flushes++;
    for (k = 0; k < HC_BINS; k++)
        for (;;) {
            c = hc_off();
            p = hc_n[k] ? hc_bin[k][--hc_n[k]] : 0;
            if (p) hc_bytes -= k * 8;
            hc_on(c);
            if (!p) break;
            hc_os_free(p, 2);
        }
}

/* r: saved r0-r5 (r0 size, r1 tag). 1: handled, r[0] the block */
static int __attribute__((used)) hc_malloc(uint32_t *r)
{
    uint32_t n = r[0], tag = r[1], need, k, c;
    void *p = 0;
    if (tag == 2 && n <= HC_MAXCAP) {
        need = ((n + 3) & ~3u) + 8;                   /* Apple's block size (0x081921b4) */
        if (need < 16) need = 16;
        need = (need + 7) & ~7u;
        k = (need - 8) / 8;
        c = hc_off();
        if (hc_n[k]) { p = hc_bin[k][--hc_n[k]]; hc_bytes -= need - 8; }
        hc_on(c);
        if (p) {
            /* charged to the task that freed it: move the charge as Apple's in-place realloc does */
            if (*(uint16_t *)((uint8_t *)p - 2) != (uint16_t)os_task_id()) {
                os_heap_lock(os_heap);
                os_recharge(os_heap, (uint8_t *)p - 8, need, 2);
                os_heap_unlock(os_heap);
                hc_moved++;
            }
            hc_hits++; r[0] = (uint32_t)p; return 1;
        }
        hc_miss++;
    }
    return 0;
}

/* Apple's allocator (heap, size, zero, tag; old block, copy, quiet) at its four call sites: on failure
 * with blocks cached they go back to the heap and it runs again (outside its heap lock). */
__attribute__((section(".text.entry"), used))
void *hc_alloc(void *heap, uint32_t n, uint32_t zero, uint32_t tag, void *old, uint32_t copy, uint32_t quiet)
{
    void *p;
    if (!hc_bytes) return os_alloc(heap, n, zero, tag, old, copy, quiet);
    p = os_alloc(heap, n, zero, tag, old, copy, 1);   /* no failure beep while a retry is still possible */
    if (!p) { hc_flush(); p = os_alloc(heap, n, zero, tag, old, copy, quiet); }
    return p;
}

/* r: saved r0-r5 (r0 block, r1 tag). 1: kept */
static int __attribute__((used)) hc_free(uint32_t *r)
{
    void *p = (void *)r[0];
    uint32_t h, cap, k, c, i;
    int kept = 0;
    if (!p || r[1] != 2) return 0;
    h = hc_hdr(p);
    if (!h || (h & 1) || *(uint16_t *)((uint8_t *)p - 4) != 2) return 0;   /* Apple's free checks it */
    cap = (h & ~0xc0000003u) - 8;
    if (cap > HC_MAXCAP || (cap & 7)) return 0;
    k = cap / 8;
    c = hc_off();
    for (i = 0; i < hc_n[k]; i++) if (hc_bin[k][i] == p) { hc_on(c); ((void (*)(void))0x0802606c)(); }   /* double free: abort */
    if (hc_n[k] < HC_PER && hc_bytes + cap <= HC_BYTES) { hc_bin[k][hc_n[k]++] = p; hc_bytes += cap; kept = 1; }
    hc_on(c);
    hc_kept += kept;
    return kept;
}

void hc_log(void)
{
    if (!(hc_hits + hc_miss)) return;
    log_s("   heap cache: "); log_d((int)hc_hits); log_s(" mallocs served ("); log_d((int)hc_moved);
    log_s(" from another task), "); log_d((int)hc_miss); log_s(" missed, "); log_d((int)hc_kept);
    log_s(" frees kept, "); log_d((int)hc_bytes); log_s(" bytes held, "); log_d((int)hc_flushes); log_s(" flushes\n");
}

#define HC_HOOK(name, cfn, back) \
__attribute__((naked, section(".text.entry"), used)) void name(void) \
{ __asm__ volatile( \
    "push {r0-r5, ip, lr}\n mov r0, sp\n bl " #cfn "\n" \
    "cmp r0, #0\n pop {r0-r5, ip, lr}\n bxne lr\n" \
    ".word 0xe92d4070\n ldr pc, 1f\n 1: .word " #back "\n"); }
HC_HOOK(hk_hcmalloc, hc_malloc, 0x080e07a8)
HC_HOOK(hk_hcfree, hc_free, 0x080dca9c)
