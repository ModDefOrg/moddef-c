// SPDX-License-Identifier: Apache-2.0

/* Command executor tests (spec §11.7), driving md_cmd_tick with synthetic
 * now_ms advances: linear step order, param/trigger writes, poll conditions
 * on raw values with timeout, length_ref-sized reads, chunked >1-PDU
 * transfers, and result assembly from bindings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moddef/moddef.h"
#include "test.h"

#define BANK 1024
static uint16_t g_holding[BANK];

/* Successive single-word reads per offset (poll sequences). */
#define SEQ_SLOTS 4
static struct {
    uint16_t off;
    uint16_t vals[4];
    int n, at;
} g_seq[SEQ_SLOTS];
static int g_seq_used;

/* Ordered write log. */
static struct {
    uint16_t off;
    uint16_t n;
} g_writes[16];
static int g_nwrites;

static void seq_set(uint16_t off, const uint16_t *vals, int n)
{
    g_seq[g_seq_used].off = off;
    memcpy(g_seq[g_seq_used].vals, vals, (size_t)n * 2);
    g_seq[g_seq_used].n = n;
    g_seq[g_seq_used].at = 0;
    g_seq_used++;
}

static void reset_banks(void)
{
    memset(g_holding, 0, sizeof g_holding);
    memset(g_seq, 0, sizeof g_seq);
    g_seq_used = 0;
    g_nwrites = 0;
}

static md_err_t mt_read_holding(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    if ((uint32_t)off + n > BANK)
        return MD_ERR_TRANSPORT;
    for (int s = 0; s < g_seq_used; s++) {
        if (g_seq[s].off == off && g_seq[s].n > 0) {
            out[0] = g_seq[s].vals[g_seq[s].at];
            if (g_seq[s].at < g_seq[s].n - 1)
                g_seq[s].at++;
            return MD_OK;
        }
    }
    for (uint16_t i = 0; i < n; i++)
        out[i] = g_holding[off + i];
    return MD_OK;
}

static md_err_t mt_read_input(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    (void)off;
    for (uint16_t i = 0; i < n; i++)
        out[i] = 0;
    return MD_OK;
}

static md_err_t mt_read_bits(void *ctx, uint16_t off, uint16_t n, bool *out)
{
    (void)ctx;
    (void)off;
    for (uint16_t i = 0; i < n; i++)
        out[i] = false;
    return MD_OK;
}

static md_err_t mt_write_holding(void *ctx, uint16_t off, uint16_t n, const uint16_t *w)
{
    (void)ctx;
    if ((uint32_t)off + n > BANK)
        return MD_ERR_TRANSPORT;
    for (uint16_t i = 0; i < n; i++)
        g_holding[off + i] = w[i];
    if (g_nwrites < 16) {
        g_writes[g_nwrites].off = off;
        g_writes[g_nwrites].n = n;
    }
    g_nwrites++;
    return MD_OK;
}

static md_err_t mt_write_coil(void *ctx, uint16_t off, bool on)
{
    (void)ctx;
    (void)off;
    (void)on;
    return MD_OK;
}

static const md_transport_t g_transport = {
    0,            mt_read_holding,  mt_read_input, mt_read_bits,
    mt_read_bits, mt_write_holding, mt_write_coil,
};

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("SKIP-FATAL: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(1);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Drive a run to completion with a synthetic clock; every WAITING_POLL
 * advances now_ms by step_ms. Returns the final status. */
static md_cmd_status_t drive(md_cmd_exec_t *ex, uint32_t step_ms, uint32_t *now_io)
{
    uint32_t now = now_io ? *now_io : 0;
    for (int guard = 0; guard < 10000; guard++) {
        md_cmd_status_t st = md_cmd_tick(ex, now);
        if (st == MD_CMD_DONE || st == MD_CMD_ERROR) {
            if (now_io)
                *now_io = now;
            return st;
        }
        if (st == MD_CMD_WAITING_POLL)
            now += step_ms;
    }
    return MD_CMD_ERROR;
}

static void test_full_cycle(md_dev_t *dev)
{
    reset_banks();
    /* busy bit clears on the 2nd poll; status goes 0 on the 2nd poll. */
    uint16_t busy[] = {1, 0};
    uint16_t stat[] = {5, 0};
    seq_set(12, busy, 2);
    seq_set(11, stat, 2);
    g_holding[20] = 2; /* result_length: 2 of the 8-word window */
    g_holding[21] = 0xDEAD;
    g_holding[22] = 0xBEEF;
    g_holding[23] = 0xFFFF; /* beyond the live length; must not be read */

    uint8_t payload[] = {1, 2, 3, 4};
    md_cmd_param_t params[2];
    memset(params, 0, sizeof params);
    params[0].field = MD_STR("mode");
    params[0].kind = MD_CMD_PARAM_VALUE;
    params[0].value = md_value_i64(7);
    params[1].field = MD_STR("payload");
    params[1].kind = MD_CMD_PARAM_BYTES;
    params[1].bytes.p = payload;
    params[1].bytes.len = 4;
    md_cmd_params_t ps = {params, 2};

    uint8_t sink[64];
    md_cmd_exec_t ex;
    OK(md_cmd_begin(&ex, dev, MD_STR("run_job"), &ps, sink, sizeof sink) == MD_OK);
    OK(drive(&ex, 10, NULL) == MD_CMD_DONE);

    /* Step wire order: payload @0 (4w), mode @5, trigger @10. */
    OK(g_nwrites == 3);
    OK(g_writes[0].off == 0 && g_writes[0].n == 4);
    OK(g_writes[1].off == 5 && g_writes[1].n == 1);
    OK(g_writes[2].off == 10 && g_writes[2].n == 1);
    OK(g_holding[0] == 0x0102 && g_holding[1] == 0x0304);
    OK(g_holding[2] == 0 && g_holding[3] == 0); /* zero-padded window */
    OK(g_holding[5] == 7);
    OK(g_holding[10] == 1);

    /* length_ref-sized read: 2 words -> 4 bytes, not the 8-word clamp. */
    md_value_t len;
    OK(md_cmd_result(&ex, MD_STR("length"), &len) == MD_OK);
    OK(len.kind == MD_VAL_U64 && len.v.u64 == 2);
    const uint8_t *data;
    size_t dlen;
    OK(md_cmd_result_data(&ex, MD_STR("data"), &data, &dlen) == MD_OK);
    OK(dlen == 4);
    OK(data[0] == 0xDE && data[1] == 0xAD && data[2] == 0xBE && data[3] == 0xEF);
}

static void test_length_ref_plain_read(md_dev_t *dev)
{
    reset_banks();
    g_holding[20] = 3;
    g_holding[21] = 0x0102;
    g_holding[22] = 0x0304;
    g_holding[23] = 0x0506;
    /* Plain md_dev-level access honours length_ref too (§11.7.1). */
    md_point_t pt;
    OK(md_devprof_find_point(&dev->prof, MD_STR("result_data"), &pt) == MD_OK);
    md_point_desc_t d;
    OK(md_point_parse(&pt, &d) == MD_OK);
    OK(d.has_length_ref);
    uint16_t words;
    OK(md_dev_point_words(dev, &d, &words) == MD_OK);
    OK(words == 3);
}

static void test_errors(md_dev_t *dev)
{
    reset_banks();
    md_cmd_exec_t ex;
    OK(md_cmd_begin(&ex, dev, MD_STR("nope"), NULL, NULL, 0) ==
       MD_ERR_COMMAND_NOT_FOUND);
    OK(md_cmd_tick(&ex, 0) == MD_CMD_ERROR);

    /* Required param missing. */
    md_cmd_param_t p;
    memset(&p, 0, sizeof p);
    p.field = MD_STR("payload");
    p.kind = MD_CMD_PARAM_BYTES;
    md_cmd_params_t ps = {&p, 1};
    OK(md_cmd_begin(&ex, dev, MD_STR("run_job"), &ps, NULL, 0) ==
       MD_ERR_PARAM_MISSING);
}

static void test_poll_timeout(md_dev_t *dev)
{
    reset_banks(); /* status stays 0; wait_forever wants 9 */
    md_cmd_exec_t ex;
    OK(md_cmd_begin(&ex, dev, MD_STR("wait_forever"), NULL, NULL, 0) == MD_OK);
    uint32_t now = 100; /* non-zero epoch: timeout math is relative */
    OK(drive(&ex, 10, &now) == MD_CMD_ERROR);
    OK(md_cmd_error(&ex) == MD_ERR_POLL_TIMEOUT);
    OK(now - 100 >= 50); /* the full 50ms budget elapsed */
}

static void test_chunked_transfers(md_dev_t *dev)
{
    reset_banks();
    static uint8_t input[400];
    static uint8_t sink[600];
    for (unsigned i = 0; i < sizeof input; i++)
        input[i] = (uint8_t)i;
    for (uint16_t i = 0; i < 300; i++)
        g_holding[500 + i] = (uint16_t)(i << 8 | (i + 1));

    md_cmd_param_t p;
    memset(&p, 0, sizeof p);
    p.field = MD_STR("input");
    p.kind = MD_CMD_PARAM_BYTES;
    p.bytes.p = input;
    p.bytes.len = sizeof input;
    md_cmd_params_t ps = {&p, 1};

    md_cmd_exec_t ex;
    OK(md_cmd_begin(&ex, dev, MD_STR("xfer"), &ps, sink, sizeof sink) == MD_OK);
    OK(drive(&ex, 10, NULL) == MD_CMD_DONE);

    /* 200-word write in <=123-word chunks: 123 + 77, second at 600+123. */
    OK(g_nwrites == 2);
    OK(g_writes[0].off == 600 && g_writes[0].n == 123);
    OK(g_writes[1].off == 723 && g_writes[1].n == 77);
    OK(g_holding[600] == 0x0001 && g_holding[601] == 0x0203);
    OK(g_holding[799] == 0x8E8F); /* bytes 398,399 fill the window exactly */

    /* 300-word read reassembled into the sink: 600 bytes. */
    const uint8_t *blob;
    size_t blen;
    OK(md_cmd_result_data(&ex, MD_STR("blob"), &blob, &blen) == MD_OK);
    OK(blen == 600);
    OK(blob[0] == 0 && blob[1] == 1); /* word 0 = 0x0001 (below the write window) */
    /* words 100..299 sit under the input write window (600..799): the read
     * must observe the freshly written param bytes. */
    OK(blob[598] == 0x8E && blob[599] == 0x8F);
}

int main(void)
{
    size_t len;
    uint8_t *buf = read_file("build/commands.moddef", &len);
    md_doc_t doc;
    OK(md_doc_init(&doc, buf, len) == MD_OK);
    md_dev_t dev;
    OK(md_dev_init(&dev, &doc, MD_STR("cmd-device"), &g_transport) == MD_OK);

    test_full_cycle(&dev);
    test_length_ref_plain_read(&dev);
    test_errors(&dev);
    test_poll_timeout(&dev);
    test_chunked_transfers(&dev);

    free(buf);
    return t_report("test_command");
}
