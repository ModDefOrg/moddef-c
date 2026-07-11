/* Generated-module tests: the growatt registry profile and the sunspec
 * test document are generated at build time (embedded binary document +
 * point-index table + typed accessors) and driven over the mock
 * transport — the C analog of the moddef-rs generated-client e2e suite. */
#include <stdio.h>

#include "growatt_sph.h"
#include "test.h"
#include "test_sunspec.h"

#define BANK 41000
static uint16_t g_holding[BANK];
static uint16_t g_input[BANK];

static md_err_t mt_read_holding(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    if ((uint32_t)off + n > BANK)
        return MD_ERR_TRANSPORT;
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
    0,           mt_read_holding,  mt_read_input, mt_read_bits,
    mt_read_bits, mt_write_holding, mt_write_coil,
};

static void test_growatt(void)
{
    md_dev_t dev;
    OK(growatt_sph_init(&dev, &g_transport) == MD_OK);
    OK(GROWATT_SPH_POINT_COUNT == 396);

    /* Enum-typed read: inverter_status @ input 0. */
    g_input[0] = 1;
    growatt_sph_inverter_run_state_t st = (growatt_sph_inverter_run_state_t)0;
    OK(growatt_sph_read_inverter_status(&dev, &st) == MD_OK);
    OK(st == GROWATT_SPH_INVERTER_RUN_STATE_NORMAL);

    /* Unknown enum values pass through losslessly. */
    g_input[0] = 99;
    OK(growatt_sph_read_inverter_status(&dev, &st) == MD_OK);
    OK((int)st == 99);

    /* Scaled decimal: pv1_voltage @ input 3, x0.1. */
    g_input[3] = 2305;
    double pv1 = 0;
    OK(growatt_sph_read_pv1_voltage(&dev, &pv1) == MD_OK);
    OK(APPROX(pv1, 230.5));

    /* Packed time slot: grid_first_slot1_start @ holding 1080. */
    g_holding[1080] = (21 << 8) | 45;
    uint64_t window = 0;
    OK(growatt_sph_read_grid_first_slot1_start(&dev, &window) == MD_OK);
    md_point_desc_t d;
    OK(growatt_sph_desc(GROWATT_SPH_GRID_FIRST_SLOT1_START, &d) == MD_OK);
    uint64_t hour = 0, minute = 0;
    OK(md_field_value(&d, window, MD_STR("hour"), &hour) == MD_OK);
    OK(md_field_value(&d, window, MD_STR("minute"), &minute) == MD_OK);
    OK(hour == 21 && minute == 45);

    /* §11.4-validated write: active_power_rate @ holding 3, 0..255. */
    OK(growatt_sph_set_active_power_rate(&dev, 50.0) == MD_OK);
    OK(g_holding[3] == 50);
    OK(growatt_sph_set_active_power_rate(&dev, 300.0) == MD_ERR_CONSTRAINT_MAX);
}

static void test_sunspec(void)
{
    md_dev_t dev;
    OK(test_sunspec_init(&dev, &g_transport) == MD_OK);

    g_holding[40000] = 0x5375;
    g_holding[40001] = 0x6e53;
    g_holding[40002] = 1;
    g_holding[40003] = 66;
    g_holding[40070] = 103;
    g_holding[40071] = 50;
    g_holding[40070 + 14] = 2301;
    g_holding[40070 + 15] = 0xffff; /* sf = -1 */
    g_holding[40122] = 0xffff;

    double w = 0;
    OK(test_sunspec_read_ac_power(&dev, &w) == MD_OK);
    OK(APPROX(w, 230.1));

    /* Sentinel surfaces as MD_ERR_UNAVAILABLE through the typed accessor. */
    g_holding[40070 + 14] = 0x8000;
    OK(test_sunspec_read_ac_power(&dev, &w) == MD_ERR_UNAVAILABLE);
}

int main(void)
{
    test_growatt();
    test_sunspec();
    return t_report("test_generated");
}
