/*
 * property_store.c - Global QWS property key/value store.
 * SPDX-License-Identifier: MIT
 */

#include "property_store.h"
#include "qws_proto.h"

#include <stdlib.h>
#include <string.h>

static void qwswl_prop_val_drop(qwswl_prop_val_t *v) { free(v->data); }

#define T qwswl_prop_map_t, int64_t, qwswl_prop_val_t
#define i_declared
#define i_no_clone
#define i_valdrop qwswl_prop_val_drop
#include "stc/hashmap.h"

void qwsprop_init(qwswl_prop_store_t *store) {
    *store = (qwswl_prop_store_t){0};
}

void qwsprop_destroy(qwswl_prop_store_t *store) {
    qwswl_prop_map_t_drop(store);
    *store = (qwswl_prop_store_t){0};
}

bool qwsprop_add(qwswl_prop_store_t *store, int32_t window, int32_t property) {
    /* _insert drops the new val (free(NULL)) when the key already exists. */
    qwswl_prop_map_t_result res = qwswl_prop_map_t_insert(
        store, PROP_KEY(window, property), (qwswl_prop_val_t){NULL, 0});
    return res.ref != NULL; /* NULL only on OOM */
}

bool qwsprop_replace_internal(qwswl_prop_store_t *store, int32_t window,
                              int32_t property, void *data, int32_t len) {
    qwswl_prop_map_t_value *entry =
        qwswl_prop_map_t_get_mut(store, PROP_KEY(window, property));
    if (!entry)
        return false;
    qwswl_prop_val_t *val = &entry->second;

    if (val->data)
        free(val->data);

    val->data = data;
    val->len = len;

    return true;
}

bool qwsprop_set(qwswl_prop_store_t *store, int32_t window, int32_t property,
                 int32_t mode, const void *data, int32_t len) {
    qwswl_prop_map_t_value *entry =
        qwswl_prop_map_t_get_mut(store, PROP_KEY(window, property));
    if (!entry)
        return false;

    qwswl_prop_val_t *val = &entry->second;

    switch (mode) {
    case QWS_PROP_REPLACE: {
        uint8_t *new_data = NULL;
        if (len > 0) {
            new_data = malloc((size_t)len);
            if (!new_data)
                return -1;
            memcpy(new_data, data, (size_t)len);
        }
        free(val->data);
        val->data = new_data;
        val->len = len > 0 ? len : 0;
        break;
    }
    case QWS_PROP_PREPEND: {
        if (len <= 0)
            break;
        int32_t new_len = len + val->len;
        uint8_t *new_data = malloc((size_t)new_len);
        if (!new_data)
            return false;
        memcpy(new_data, data, (size_t)len);
        if (val->len > 0)
            memcpy(new_data + len, val->data, (size_t)val->len);
        free(val->data);
        val->data = new_data;
        val->len = new_len;
        break;
    }
    case QWS_PROP_APPEND: {
        if (len <= 0)
            break;
        int32_t new_len = val->len + len;
        uint8_t *new_data = realloc(val->data, (size_t)new_len);
        if (!new_data)
            return false;
        memcpy(new_data + val->len, data, (size_t)len);
        val->data = new_data;
        val->len = new_len;
        break;
    }
    default:
        return false;
    }

    return true;
}

bool qwsprop_remove(qwswl_prop_store_t *store, int32_t window,
                    int32_t property) {
    /* _erase calls i_valdrop automatically — no manual free needed. */
    return qwswl_prop_map_t_erase(store, PROP_KEY(window, property));
}

bool qwsprop_get(const qwswl_prop_store_t *store, int32_t window,
                 int32_t property, const void **data_out, int32_t *len_out) {
    const qwswl_prop_map_t_value *entry =
        qwswl_prop_map_t_get(store, PROP_KEY(window, property));
    if (!entry) {
        *data_out = NULL;
        *len_out = -1;
        return false;
    }
    *data_out = entry->second.data;
    *len_out = entry->second.len;
    return true;
}
