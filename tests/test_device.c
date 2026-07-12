// SPDX-License-Identifier: Apache-2.0

/* Device layer tests (spec §32.4) over binary documents and a mock
 * transport: reads/scaling, sentinels, constrained writes, and SunSpec
 * discovery with ID-relative model offsets (§7.3) + base caching. */
#include <stdio.h>
#include <stdlib.h>

#include "moddef/moddef.h"
#include "test.h"

/* In-memory register banks; reads beyond size fail like a device
 * answering a Modbus exception (used to skip discovery anchors). */
#define BANK 41000
static uint16_t g_holding[BANK];
static uint16_t g_input[BANK];
static int g_anchor_probes; /* reads of holding[40000] x2 */

static md_err_t mt_read_holding(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    if ((uint32_t)off + n > BANK)
        return MD_ERR_TRANSPORT;
    if (off == 40000 && n == 2)
        g_anchor_probes++;
    for (uint16_t i = 0; i < n; i++)
        out[i] = g_holding[off + i];
    return MD_OK;
}

static md_err_t mt_read_input(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    if ((uint32_t)off + n > BANK)
        return MD_ERR_TRANSPORT;
    for (uint16_t i = 0; i < n; i++)
        out[i] = g_input[off + i];
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
    0,           mt_read_holding, mt_read_input, mt_read_bits,
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

static void test_meter(void)
{
    size_t len;
    uint8_t *buf = read_file("build/meter.moddef", &len);
    md_doc_t doc;
    OK(md_doc_init(&doc, buf, len) == MD_OK);
    md_dev_t dev;
    OK(md_dev_init(&dev, &doc, MD_STR("meter"), &g_transport) == MD_OK);

    /* Reads + scaling across input registers. */
    g_input[0] = 2305;
    md_value_t v;
    OK(md_dev_read(&dev, MD_STR("voltage_l1"), &v) == MD_OK);
    OK(v.kind == MD_VAL_F64 && APPROX(v.v.f64, 230.5));

    /* §8.4 sentinel with meaning. */
    g_input[1] = 0xffff;
    OK(md_dev_read(&dev, MD_STR("voltage_l2"), &v) == MD_OK);
    OK(v.kind == MD_VAL_UNAVAILABLE);
    OK(md_str_eq(v.na_meaning, MD_STR("not_implemented")));

    /* Unknown point id. */
    OK(md_dev_read(&dev, MD_STR("nope"), &v) == MD_ERR_NOT_FOUND);

    /* §11.4-validated writes. */
    OK(md_dev_write_f64(&dev, MD_STR("stop_soc"), 80.0) == MD_OK);
    OK(g_holding[0] == 80);
    OK(md_dev_write_f64(&dev, MD_STR("stop_soc"), 101.0) == MD_ERR_CONSTRAINT_MAX);
    OK(md_dev_write_f64(&dev, MD_STR("stop_soc"), -1.0) == MD_ERR_CONSTRAINT_MIN);
    OK(md_dev_write_f64(&dev, MD_STR("stop_soc"), 50.5) == MD_ERR_CONSTRAINT_STEP);

    /* Inverse transform on write. */
    OK(md_dev_write_f64(&dev, MD_STR("setpoint_scaled"), 23.5) == MD_OK);
    OK(g_holding[2] == 235);

    /* Read-only points are rejected. */
    OK(md_dev_write_f64(&dev, MD_STR("voltage_l1"), 1.0) == MD_ERR_NOT_WRITABLE);

    free(buf);
}

static void test_sunspec(void)
{
    size_t len;
    uint8_t *buf = read_file("build/sunspec.moddef", &len);
    md_doc_t doc;
    OK(md_doc_init(&doc, buf, len) == MD_OK);
    md_dev_t dev;
    OK(md_dev_init(&dev, &doc, MD_STR("inv"), &g_transport) == MD_OK);

    g_holding[40000] = 0x5375; /* "Su" */
    g_holding[40001] = 0x6e53; /* "nS" */
    g_holding[40002] = 1;      /* model 1 header */
    g_holding[40003] = 66;
    g_holding[40070] = 103; /* model 103 header (= 40002 + 2 + 66) */
    g_holding[40071] = 50;
    g_holding[40070 + 14] = 2301;   /* W */
    g_holding[40070 + 15] = 0xffff; /* W_SF = -1 */
    g_holding[40122] = 0xffff;      /* end of chain */

    md_value_t v;
    OK(md_dev_read(&dev, MD_STR("ac_power"), &v) == MD_OK);
    OK(v.kind == MD_VAL_F64 && APPROX(v.v.f64, 230.1));

    /* Model base is cached: a second read must not re-probe the anchor. */
    int probes = g_anchor_probes;
    OK(md_dev_read(&dev, MD_STR("ac_power"), &v) == MD_OK);
    OK(g_anchor_probes == probes);

    /* §8.4 sentinel through discovery + scale_ref path. */
    g_holding[40070 + 14] = 0x8000;
    OK(md_dev_read(&dev, MD_STR("ac_power"), &v) == MD_OK);
    OK(v.kind == MD_VAL_UNAVAILABLE);
    OK(md_str_eq(v.na_meaning, MD_STR("not_implemented")));

    free(buf);
}

int main(void)
{
    test_meter();
    test_sunspec();
    return t_report("test_device");
}
