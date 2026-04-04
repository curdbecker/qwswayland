/*
 * property_store.h - Global QWS property key/value store.
 * SPDX-License-Identifier: MIT
 */

#ifndef PROPERTY_STORE_H
#define PROPERTY_STORE_H

#include "stc/types.h"

#include <stdbool.h>
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

/* Pack (window, property) into a single int64_t key. */
#define PROP_KEY(w, p) ((int64_t)(uint32_t)(w) << 32 | (uint32_t)(p))

void qwsprop_init(qwswl_prop_store_t *store);
void qwsprop_destroy(qwswl_prop_store_t *store);

/* Idempotent if (window, property) already exists. Returns false on OOM. */
bool qwsprop_add(qwswl_prop_store_t *store, int32_t window, int32_t property);

bool qwsprop_replace_internal(qwswl_prop_store_t *store, int32_t window,
                              int32_t property, void *data, int32_t len);

/* Returns false if property not found or OOM. */
bool qwsprop_set(qwswl_prop_store_t *store, int32_t window, int32_t property,
                 int32_t mode, const void *data, int32_t len);

/* Returns false if not found. */
bool qwsprop_remove(qwswl_prop_store_t *store, int32_t window,
                    int32_t property);

/* Returns false and set *data_out = NULL / *len_out = -1 when not found. */
bool qwsprop_get(const qwswl_prop_store_t *store, int32_t window,
                 int32_t property, const void **data_out, int32_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* PROPERTY_STORE_H */
