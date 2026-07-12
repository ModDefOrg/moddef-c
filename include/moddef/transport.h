// SPDX-License-Identifier: Apache-2.0

/* Transport abstraction (spec §32.3): blocking register-level access as a
 * table of function pointers — adapt to your HAL / Modbus stack / RTOS.
 *
 * `offset` is the zero-based data-model offset within the address space
 * (spec §7.2); implementations apply any unit/base-address mapping. Return
 * MD_OK or MD_ERR_TRANSPORT (a device exception answer is also
 * MD_ERR_TRANSPORT at this layer). One in-flight request per transport is
 * the caller's discipline — Modbus is a serial request/response channel. */
#ifndef MODDEF_TRANSPORT_H
#define MODDEF_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "moddef/err.h"

typedef struct md_transport {
    void *ctx;
    md_err_t (*read_holding)(void *ctx, uint16_t offset, uint16_t count, uint16_t *out);
    md_err_t (*read_input)(void *ctx, uint16_t offset, uint16_t count, uint16_t *out);
    md_err_t (*read_coils)(void *ctx, uint16_t offset, uint16_t count, bool *out);
    md_err_t (*read_discrete)(void *ctx, uint16_t offset, uint16_t count, bool *out);
    md_err_t (*write_holding)(void *ctx, uint16_t offset, uint16_t count,
                              const uint16_t *words);
    md_err_t (*write_coil)(void *ctx, uint16_t offset, bool on);
} md_transport_t;

#endif /* MODDEF_TRANSPORT_H */
