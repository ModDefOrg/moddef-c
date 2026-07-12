// SPDX-License-Identifier: Apache-2.0

/* Minimal integration sketch: a generated Growatt client polled from a
 * bare-metal superloop. The transport wraps whatever Modbus RTU/TCP stack
 * your firmware already has — here a stub that returns fixed data so the
 * example compiles and runs on the host:
 *
 *   make examples && ./build/superloop
 */
#include <stdio.h>

#include "growatt_sph.h"

/* --- transport: adapt these six calls to your Modbus stack ---------------- */

static md_err_t read_holding(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    (void)off;
    for (uint16_t i = 0; i < n; i++)
        out[i] = 0; /* modbus_rtu_read_holding(uart, unit_id, off, n, out) */
    return MD_OK;
}

static md_err_t read_input(void *ctx, uint16_t off, uint16_t n, uint16_t *out)
{
    (void)ctx;
    for (uint16_t i = 0; i < n; i++)
        out[i] = 0;
    if (off == 3 && n == 1)
        out[0] = 2305; /* pretend PV1 is at 230.5 V */
    return MD_OK;
}

static md_err_t read_bits(void *ctx, uint16_t off, uint16_t n, bool *out)
{
    (void)ctx;
    (void)off;
    for (uint16_t i = 0; i < n; i++)
        out[i] = false;
    return MD_OK;
}

static md_err_t write_holding(void *ctx, uint16_t off, uint16_t n, const uint16_t *w)
{
    (void)ctx;
    (void)off;
    (void)n;
    (void)w;
    return MD_OK;
}

static md_err_t write_coil(void *ctx, uint16_t off, bool on)
{
    (void)ctx;
    (void)off;
    (void)on;
    return MD_OK;
}

static const md_transport_t transport = {
    0, read_holding, read_input, read_bits, read_bits, write_holding, write_coil,
};

/* --- application ------------------------------------------------------------ */

int main(void)
{
    md_dev_t inverter;
    if (growatt_sph_init(&inverter, &transport) != MD_OK)
        return 1;

    for (int tick = 0; tick < 3; tick++) { /* while (1) on real hardware */
        growatt_sph_inverter_run_state_t state;
        double pv1 = 0;

        if (growatt_sph_read_inverter_status(&inverter, &state) == MD_OK)
            printf("state=%d\n", (int)state);
        if (growatt_sph_read_pv1_voltage(&inverter, &pv1) == MD_OK)
            printf("pv1=%.1f V\n", pv1);

        /* Constrained write (§11.4 validated before it hits the wire): */
        if (growatt_sph_set_active_power_rate(&inverter, 80.0) != MD_OK)
            printf("power rate write rejected\n");
    }
    return 0;
}
