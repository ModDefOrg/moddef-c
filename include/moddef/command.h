// SPDX-License-Identifier: Apache-2.0

/* Command executor (spec §11.7): multi-step register procedures — write
 * caller params, arm a trigger register, poll a status register until a
 * condition holds, read results whose size may only be known at runtime
 * (length_ref, §11.7.1).
 *
 * Tick-driven, no allocation, no timers: the caller drives
 *
 *   md_cmd_exec_t ex;
 *   md_cmd_begin(&ex, &dev, MD_STR("start_transaction"), &params, sink, cap);
 *   while (md_cmd_tick(&ex, now_ms()) < MD_CMD_DONE)
 *       ;  // yield to your scheduler; WAITING_POLL means "call later"
 *
 * from its own scheduler, supplying a monotonic millisecond clock. The
 * library never sleeps and never reads a wall clock — poll interval_ms /
 * timeout_ms are tracked against the caller-supplied now_ms, so runs are
 * deterministic and unit-testable.
 *
 * Transport writes/reads larger than one Modbus PDU are chunked
 * (≤123-word writes; reads chunk at MD_MAX_POINT_WORDS). Chunked
 * string/bytes transfers require word-big order (the overwhelming norm).
 *
 * String/bytes read-step results land **untrimmed** in the caller's data
 * sink (the raw register window bytes); numeric results are md_value_t.
 * Params reference caller storage and must outlive the run. */
#ifndef MODDEF_COMMAND_H
#define MODDEF_COMMAND_H

#include "moddef/device.h"

/* Read-step bindings held per run (ReadStep.into slots). */
#ifndef MD_CMD_MAX_BINDINGS
#define MD_CMD_MAX_BINDINGS 4
#endif

/* Modbus single-PDU practical write cap (FC16). */
#ifndef MD_CMD_MAX_WRITE_WORDS
#define MD_CMD_MAX_WRITE_WORDS 123
#endif

/* Default poll interval when a PollStep omits interval_ms. */
#ifndef MD_CMD_DEFAULT_INTERVAL_MS
#define MD_CMD_DEFAULT_INTERVAL_MS 250
#endif

typedef enum md_cmd_status {
    MD_CMD_RUNNING = 0,   /* made progress; tick again when convenient */
    MD_CMD_WAITING_POLL,  /* poll interval pending; tick again later */
    MD_CMD_DONE,
    MD_CMD_ERROR          /* inspect md_cmd_error() */
} md_cmd_status_t;

typedef enum md_cmd_param_kind {
    MD_CMD_PARAM_VALUE = 0, /* numeric md_value_t */
    MD_CMD_PARAM_STR,       /* STRING_ASCII / STRING_UTF8 param */
    MD_CMD_PARAM_BYTES      /* BYTES_RAW param */
} md_cmd_param_kind_t;

typedef struct md_cmd_param {
    md_str_t field;   /* CommandParam.field this value binds to */
    uint8_t kind;     /* md_cmd_param_kind_t */
    md_value_t value; /* MD_CMD_PARAM_VALUE */
    md_str_t str;     /* MD_CMD_PARAM_STR */
    md_bytes_t bytes; /* MD_CMD_PARAM_BYTES */
} md_cmd_param_t;

typedef struct md_cmd_params {
    const md_cmd_param_t *items;
    uint8_t n;
} md_cmd_params_t;

typedef struct md_cmd_binding {
    md_str_t name;  /* ReadStep.into */
    bool is_data;   /* payload lives in the data sink */
    md_value_t value;
    uint32_t data_off;
    uint32_t data_len;
} md_cmd_binding_t;

typedef struct md_cmd_exec {
    md_dev_t *dev;
    md_bytes_t cmd_raw;      /* the Command message */
    md_cmd_params_t params;  /* caller storage */
    md_wire_t steps;         /* cursor over cmd_raw's step frames */
    uint8_t status;          /* md_cmd_status_t */
    md_err_t err;

    bool in_poll;
    bool poll_first;         /* evaluate immediately on the entering tick */
    md_bytes_t poll_raw;     /* current PollStep message */
    uint32_t poll_started_ms;
    uint32_t poll_due_ms;

    md_cmd_binding_t bindings[MD_CMD_MAX_BINDINGS];
    uint8_t n_bindings;

    uint8_t *data;           /* caller sink for string/bytes read steps */
    uint32_t data_cap;
    uint32_t data_used;
} md_cmd_exec_t;

/* Locate command_id in the device's profile, validate that every required
 * param is supplied, and arm the executor. data_buf/data_cap receive
 * string/bytes read-step payloads (pass NULL/0 when the command has none). */
md_err_t md_cmd_begin(md_cmd_exec_t *ex, md_dev_t *dev, md_str_t command_id,
                      const md_cmd_params_t *params, uint8_t *data_buf,
                      size_t data_cap);

/* Advance the run: executes steps until it completes (MD_CMD_DONE), fails
 * (MD_CMD_ERROR), or reaches a poll whose interval has not elapsed
 * (MD_CMD_WAITING_POLL). now_ms is any monotonic millisecond clock. */
md_cmd_status_t md_cmd_tick(md_cmd_exec_t *ex, uint32_t now_ms);

/* The failure cause after MD_CMD_ERROR (MD_OK otherwise). */
md_err_t md_cmd_error(const md_cmd_exec_t *ex);

/* Fetch a numeric result by CommandResult.field after MD_CMD_DONE. */
md_err_t md_cmd_result(const md_cmd_exec_t *ex, md_str_t field, md_value_t *out);

/* Fetch a string/bytes result: a view into the caller's data sink. */
md_err_t md_cmd_result_data(const md_cmd_exec_t *ex, md_str_t field,
                            const uint8_t **p, size_t *len);

#endif /* MODDEF_COMMAND_H */
