//go:build qat

/*
 * JuiceFS, Copyright 2026 Juicedata, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* QAT (qatlib) LZ4 *block* compress/decompress, format-compatible with liblz4
 * LZ4_compress_default / LZ4_decompress_safe.
 *
 * Hardware LZ4 output is a sequence of "[u32 size][block]" entries (the LZ4
 * frame block layout, without frame header / end mark). The hardware cuts the
 * input into independent 64 KiB blocks whatever lz4BlockMaxSize says, so
 * merge_blocks() rewrites them into one raw LZ4 block. Decompression only
 * re-adds the 4-byte size word.
 *
 * Data is copied through per-instance DMA buffers (qatlib usdm); each call
 * takes one free instance, submits, and polls that instance until done.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qat/cpa.h>
#include <qat/cpa_dc.h>
#include <qat/icp_sal_poll.h>
#include <qat/icp_sal_user.h>
#include <qat/qae_mem.h>

#include "qat_lz4.h"

#define QLZ4_MAX_INST 64
#define STORED_FLAG   0x80000000u

/* Largest request (JFS_QAT_LZ4_MAX_KB); bigger blocks stay on software LZ4. */
static unsigned g_max_in = 4u << 20;
#define QLZ4_MAX_IN   g_max_in
#define QLZ4_BUF_SZ   (g_max_in + g_max_in / 255 + 64)

struct qinst {
    pthread_mutex_t lock;
    CpaInstanceHandle h;
    CpaDcSessionHandle comp, decomp;
    CpaBufferList *src, *dst;
    CpaDcRqResults res;
    volatile int done;
    int node;
};

static struct qinst g_inst[QLZ4_MAX_INST];
static int g_ninst;
static unsigned g_rr;
static int g_poll_sleep_ns = 2000;
static struct qlz4_stats g_stats;

static void dc_cb(void *tag, CpaStatus status)
{
    (void)status;
    ((struct qinst *)tag)->done = 1;
}

static void log_err(int compress, const char *what, int code)
{
    static int n;
    if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 5)
        fprintf(stderr, "qat_lz4: %s %s failed: %d\n", compress ? "compress" : "decompress", what, code);
}

static CpaBufferList *alloc_bl(CpaInstanceHandle h, int node, Cpa32U size)
{
    Cpa32U meta = 0;
    if (cpaDcBufferListGetMetaSize(h, 1, &meta) != CPA_STATUS_SUCCESS)
        return NULL;
    CpaBufferList *bl = qaeMemAllocNUMA(sizeof(*bl) + sizeof(CpaFlatBuffer), node, 64);
    if (!bl)
        return NULL;
    bl->numBuffers = 1;
    bl->pBuffers = (CpaFlatBuffer *)(bl + 1);
    bl->pPrivateMetaData = meta ? qaeMemAllocNUMA(meta, node, 64) : NULL;
    bl->pBuffers[0].pData = qaeMemAllocNUMA(size, node, 64);
    bl->pBuffers[0].dataLenInBytes = size;
    if (!bl->pBuffers[0].pData || (meta && !bl->pPrivateMetaData))
        return NULL;
    return bl;
}

static CpaDcSessionHandle make_session(struct qinst *qi, CpaDcSessionDir dir, int level)
{
    CpaDcSessionSetupData sd;
    Cpa32U sess_sz = 0, ctx_sz = 0;
    memset(&sd, 0, sizeof(sd));
    sd.compLevel = level;
    sd.compType = CPA_DC_LZ4;
    sd.huffType = CPA_DC_HT_STATIC;
    sd.autoSelectBestHuffmanTree = CPA_DC_ASB_DISABLED;
    sd.sessDirection = dir;
    sd.sessState = CPA_DC_STATELESS;
    sd.checksum = CPA_DC_XXHASH32;
    sd.lz4BlockMaxSize = CPA_DC_LZ4_MAX_BLOCK_SIZE_4M;
    sd.lz4BlockChecksum = CPA_FALSE;
    sd.lz4BlockIndependence = CPA_TRUE;
    sd.minMatch = CPA_DC_MIN_4_BYTE_MATCH;
    if (cpaDcGetSessionSize(qi->h, &sd, &sess_sz, &ctx_sz) != CPA_STATUS_SUCCESS)
        return NULL;
    CpaDcSessionHandle s = qaeMemAllocNUMA(sess_sz, qi->node, 64);
    if (!s || cpaDcInitSession(qi->h, s, &sd, NULL, dc_cb) != CPA_STATUS_SUCCESS)
        return NULL;
    return s;
}

int qlz4_init(int level)
{
    Cpa16U n = 0;
    CpaInstanceHandle hs[QLZ4_MAX_INST];
    const char *e = getenv("JFS_QAT_LZ4_POLL_NS");

    if (g_ninst)
        return g_ninst;
    if (e)
        g_poll_sleep_ns = atoi(e);
    if ((e = getenv("JFS_QAT_LZ4_MAX_KB")))
        g_max_in = atoi(e) << 10;
    int max_inst = (e = getenv("JFS_QAT_LZ4_INSTANCES")) ? atoi(e) : QLZ4_MAX_INST;
    if (qaeMemInit() != CPA_STATUS_SUCCESS)
        return -1;
    if (icp_sal_userStartMultiProcess("SSL", CPA_FALSE) != CPA_STATUS_SUCCESS)
        return -2;
    if (cpaDcGetNumInstances(&n) != CPA_STATUS_SUCCESS || n == 0)
        return -3;
    if (n > max_inst)
        n = max_inst;
    if (n > QLZ4_MAX_INST)
        n = QLZ4_MAX_INST;
    if (cpaDcGetInstances(n, hs) != CPA_STATUS_SUCCESS)
        return -4;
    for (int i = 0; i < n; i++) {
        struct qinst *qi = &g_inst[g_ninst];
        CpaInstanceInfo2 info;
        memset(qi, 0, sizeof(*qi));
        qi->h = hs[i];
        qi->node = cpaDcInstanceGetInfo2(qi->h, &info) == CPA_STATUS_SUCCESS ? info.nodeAffinity : 0;
        if (cpaDcSetAddressTranslation(qi->h, (CpaVirtualToPhysical)qaeVirtToPhysNUMA) != CPA_STATUS_SUCCESS ||
            cpaDcStartInstance(qi->h, 0, NULL) != CPA_STATUS_SUCCESS)
            continue;
        qi->comp = make_session(qi, CPA_DC_DIR_COMPRESS, level);
        qi->decomp = make_session(qi, CPA_DC_DIR_DECOMPRESS, level);
        qi->src = alloc_bl(qi->h, qi->node, QLZ4_BUF_SZ);
        qi->dst = alloc_bl(qi->h, qi->node, QLZ4_BUF_SZ);
        if (!qi->comp || !qi->decomp || !qi->src || !qi->dst)
            continue;
        pthread_mutex_init(&qi->lock, NULL);
        g_ninst++;
    }
    return g_ninst ? g_ninst : -5;
}

/* Literal-length / match-length extension bytes: 255 255 ... rest. */
static inline int read_len(const uint8_t **p, const uint8_t *end, int base)
{
    int len = base;
    if (base == 15) {
        uint8_t b;
        do {
            if (*p >= end)
                return -1;
            b = *(*p)++;
            len += b;
        } while (b == 255);
    }
    return len;
}

static inline uint8_t *write_lit_len(uint8_t *o, int len)
{
    for (len -= 15; len >= 255; len -= 255)
        *o++ = 255;
    *o++ = len;
    return o;
}

#define MAX_SEGS 128
struct seg { const uint8_t *p; int len; };

/* Emit one sequence whose literals are carry[] + (lit, lit_len); `tail` is the
 * offset + match-length bytes copied verbatim, and `mnib` the match nibble. */
static uint8_t *emit(uint8_t *o, struct seg *carry, int *ncarry, int carry_len, const uint8_t *lit,
                     int lit_len, int mnib, const uint8_t *tail, int tail_len)
{
    int total = carry_len + lit_len;
    *o++ = (total >= 15 ? 15 << 4 : total << 4) | mnib;
    if (total >= 15)
        o = write_lit_len(o, total);
    for (int i = 0; i < *ncarry; i++) {
        memcpy(o, carry[i].p, carry[i].len);
        o += carry[i].len;
    }
    *ncarry = 0;
    if (lit_len)
        memcpy(o, lit, lit_len);
    o += lit_len;
    if (tail_len)
        memcpy(o, tail, tail_len);
    return o + tail_len;
}

/* Hardware output "[u32 size][block]..." -> one raw LZ4 block in out.
 * Every hardware block is independent and ends with a literals-only
 * sequence (stored blocks are all literals); that trailing run is carried and
 * merged into the next block's first sequence, so match offsets stay valid. */
static int merge_blocks(const uint8_t *in, int in_len, uint8_t *out, int out_cap)
{
    struct seg carry[MAX_SEGS];
    int ncarry = 0, carry_len = 0, off = 0;
    uint8_t *o = out;
    const uint8_t *oend = out + out_cap - 32;

    while (off + 4 <= in_len) {
        uint32_t w;
        memcpy(&w, in + off, 4);
        int blen = w & ~STORED_FLAG;
        const uint8_t *p = in + off + 4, *end = p + blen;
        off += 4 + blen;
        if (blen == 0)
            continue;
        if (off > in_len || ncarry >= MAX_SEGS)
            return -1;
        if (w & STORED_FLAG) {
            carry[ncarry++] = (struct seg){p, blen};
            carry_len += blen;
            continue;
        }
        int first = 1;
        const uint8_t *run_start = p; /* sequences still to be copied verbatim */
        while (p < end) {
            const uint8_t *s = p;
            uint8_t tok = *p++;
            int lit = read_len(&p, end, tok >> 4);
            if (lit < 0 || p + lit > end)
                return -1;
            const uint8_t *lp = p;
            p += lit;
            if (p == end) { /* last sequence: literals only */
                if (ncarry >= MAX_SEGS || o + (s - run_start) > oend)
                    return -1;
                memcpy(o, run_start, s - run_start);
                o += s - run_start;
                carry[ncarry++] = (struct seg){lp, lit};
                carry_len += lit;
                break;
            }
            const uint8_t *tail = p;
            if (p + 2 > end)
                return -1;
            p += 2;
            if (read_len(&p, end, tok & 15) < 0)
                return -1;
            if (first && ncarry) {
                if (o + carry_len + lit + (p - tail) + 16 > oend)
                    return -1;
                o = emit(o, carry, &ncarry, carry_len, lp, lit, tok & 15, tail, p - tail);
                carry_len = 0;
                run_start = p;
            }
            first = 0;
        }
    }
    if (o + carry_len + 16 > oend + 32)
        return -1;
    /* final literals-only sequence */
    return emit(o, carry, &ncarry, carry_len, NULL, 0, 0, NULL, 0) - out;
}

static struct qinst *get_inst(void)
{
    unsigned start = __atomic_fetch_add(&g_rr, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < g_ninst; i++) {
        struct qinst *qi = &g_inst[(start + i) % g_ninst];
        if (pthread_mutex_trylock(&qi->lock) == 0)
            return qi;
    }
    struct qinst *qi = &g_inst[start % g_ninst];
    pthread_mutex_lock(&qi->lock);
    return qi;
}

/* Submit on qi (lock held) and poll until the callback fires. */
static int run(struct qinst *qi, int compress, Cpa32U in_len)
{
    CpaDcOpData op;
    CpaStatus st;
    memset(&op, 0, sizeof(op));
    op.flushFlag = CPA_DC_FLUSH_FINAL;
    op.compressAndVerify = compress ? CPA_TRUE : CPA_FALSE;
    qi->src->pBuffers[0].dataLenInBytes = in_len;
    qi->dst->pBuffers[0].dataLenInBytes = QLZ4_BUF_SZ;
    memset(&qi->res, 0, sizeof(qi->res));
    qi->done = 0;
    do {
        st = compress ? cpaDcCompressData2(qi->h, qi->comp, qi->src, qi->dst, &op, &qi->res, qi)
                      : cpaDcDecompressData2(qi->h, qi->decomp, qi->src, qi->dst, &op, &qi->res, qi);
        if (st == CPA_STATUS_RETRY)
            icp_sal_DcPollInstance(qi->h, 0);
    } while (st == CPA_STATUS_RETRY);
    if (st != CPA_STATUS_SUCCESS) {
        log_err(compress, "submit", st);
        return -1;
    }
    struct timespec ts = {0, g_poll_sleep_ns};
    while (!qi->done) {
        icp_sal_DcPollInstance(qi->h, 0);
        if (!qi->done && g_poll_sleep_ns > 0)
            nanosleep(&ts, NULL);
    }
    if (qi->res.status != CPA_DC_OK) {
        log_err(compress, "result", qi->res.status);
        return -2;
    }
    return 0;
}

int qlz4_compress(const void *src, int src_len, void *dst, int dst_cap)
{
    if (!g_ninst || src_len <= 0 || (unsigned)src_len > QLZ4_MAX_IN)
        return -1;
    struct qinst *qi = get_inst();
    int ret = -1;
    memcpy(qi->src->pBuffers[0].pData, src, src_len);
    if (run(qi, 1, src_len) == 0 && qi->res.consumed == (Cpa32U)src_len)
        ret = merge_blocks(qi->dst->pBuffers[0].pData, qi->res.produced, dst, dst_cap);
    pthread_mutex_unlock(&qi->lock);
    __atomic_fetch_add(ret > 0 ? &g_stats.comp_ok : &g_stats.comp_fallback, 1, __ATOMIC_RELAXED);
    return ret;
}

int qlz4_decompress(const void *src, int src_len, void *dst, int dst_cap)
{
    if (!g_ninst || src_len <= 0 || (unsigned)src_len + 4 > QLZ4_BUF_SZ)
        return -1;
    struct qinst *qi = get_inst();
    int ret = -1;
    uint8_t *in = qi->src->pBuffers[0].pData;
    uint32_t blk = src_len;
    memcpy(in, &blk, 4);
    memcpy(in + 4, src, src_len);
    if (run(qi, 0, src_len + 4) == 0 && (int)qi->res.produced <= dst_cap) {
        memcpy(dst, qi->dst->pBuffers[0].pData, qi->res.produced);
        ret = qi->res.produced;
    }
    pthread_mutex_unlock(&qi->lock);
    __atomic_fetch_add(ret >= 0 ? &g_stats.decomp_ok : &g_stats.decomp_fallback, 1, __ATOMIC_RELAXED);
    return ret;
}

void qlz4_get_stats(struct qlz4_stats *s)
{
    *s = g_stats;
}
