/*
 * property_store.h - Global QWS property key/value store.
 * SPDX-License-Identifier: MIT
 */

#ifndef PROPERTY_STORE_H
#define PROPERTY_STORE_H

#include "stc/types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data; /* NULL when len == 0 */
    int32_t len;
} qwswl_prop_val_t;

declare_hashmap(qwswl_prop_map_t, int64_t, qwswl_prop_val_t);
typedef qwswl_prop_map_t qwswl_prop_store_t;

void qwsprop_init(qwswl_prop_store_t *store);
void qwsprop_destroy(qwswl_prop_store_t *store);

/* Returns 0 on success, -1 on OOM. Idempotent if (window, property) already
 * exists. */
int qwsprop_add(qwswl_prop_store_t *store, int32_t window, int32_t property);

int qwsprop_replace_internal(qwswl_prop_store_t *store, int32_t window,
                             int32_t property, void *data, int32_t len);

/* mode: 0=Replace, 1=Prepend, 2=Append. Returns 0 or -1 (not found / OOM). */
int qwsprop_set(qwswl_prop_store_t *store, int32_t window, int32_t property,
                int32_t mode, const void *data, int32_t len);

/* Returns 0 if removed, -1 if not found. */
int qwsprop_remove(qwswl_prop_store_t *store, int32_t window, int32_t property);

/* Returns 0 and sets *data_out / *len_out on success.
 * Returns -1 and sets *len_out = -1 when not found. */
int qwsprop_get(const qwswl_prop_store_t *store, int32_t window,
                int32_t property, const void **data_out, int32_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* PROPERTY_STORE_H */
