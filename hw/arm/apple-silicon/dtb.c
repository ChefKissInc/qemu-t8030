/*
 *
 * Copyright (c) 2019 Jonathan Afek <jonyafek@me.com>
 * Copyright (c) 2024 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"

#define DT_PROP_NAME_LEN (32)
#define DT_PROP_PLACEHOLDER (1 << 31)

static void dtb_prop_destroy(gpointer data)
{
    DTBProp *prop;

    prop = data;

    g_assert_nonnull(prop);
    g_free(prop->data);
    g_free(prop);
}

static DTBNode *dtb_new_node(void)
{
    DTBNode *node;

    node = g_new0(DTBNode, 1);

    node->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        dtb_prop_destroy);

    return node;
}

DTBNode *dtb_create_node(DTBNode *parent, const char *name)
{
    DTBNode *node;

    node = dtb_new_node();

    if (name != NULL) {
        dtb_set_prop(node, "name", strlen(name), name);
    }

    if (parent != NULL) {
        g_assert_null(dtb_get_node(parent, name));
        parent->children = g_list_append(parent->children, node);
    }

    return node;
}

static DTBProp *dtb_deserialise_prop(uint8_t **dtb_blob, char **name)
{
    g_assert_nonnull(dtb_blob);
    g_assert_nonnull(*dtb_blob);
    g_assert_nonnull(name);

    DTBProp *prop;
    size_t name_len;

    prop = g_new0(DTBProp, 1);
    name_len = strnlen((char *)*dtb_blob, DT_PROP_NAME_LEN);
    *name = g_new(char, name_len + 1);
    memcpy(*name, *dtb_blob, name_len);
    (*name)[name_len] = '\0';
    *dtb_blob += DT_PROP_NAME_LEN;

    prop->length = ldl_le_p(*dtb_blob);
    if (prop->length & DT_PROP_PLACEHOLDER) {
        prop->placeholder = true;
        prop->length &= ~DT_PROP_PLACEHOLDER;
    }
    *dtb_blob += sizeof(uint32_t);

    if (prop->length != 0) {
        prop->data = g_malloc0(prop->length);
        g_assert_nonnull(prop->data);
        memcpy(prop->data, *dtb_blob, prop->length);
        *dtb_blob += ROUND_UP(prop->length, 4);
    }

    return prop;
}

static DTBNode *dtb_deserialise_node(uint8_t **dtb_blob)
{
    uint32_t i;
    DTBNode *node;
    uint32_t prop_count;
    uint32_t children_count;
    DTBNode *child;
    DTBProp *prop;
    char *key;

    g_assert_nonnull(dtb_blob);
    g_assert_nonnull(*dtb_blob);

    node = dtb_new_node();
    prop_count = ldl_le_p(*dtb_blob);
    *dtb_blob += sizeof(prop_count);
    children_count = ldl_le_p(*dtb_blob);
    *dtb_blob += sizeof(children_count);

    for (i = 0; i < prop_count; i++) {
        prop = dtb_deserialise_prop(dtb_blob, &key);
        g_assert_nonnull(prop);
        g_assert_nonnull(key);
        g_assert_true(g_hash_table_insert(node->props, key, prop));
    }

    for (i = 0; i < children_count; i++) {
        child = dtb_deserialise_node(dtb_blob);
        g_assert_nonnull(child);
        node->children = g_list_append(node->children, child);
    }

    return node;
}

DTBNode *dtb_deserialise(uint8_t *dtb_blob)
{
    return dtb_deserialise_node(&dtb_blob);
}

static void dtb_destroy_node(DTBNode *node)
{
    g_assert_nonnull(node);

    g_hash_table_unref(node->props);

    if (node->children != NULL) {
        g_list_free_full(node->children, (GDestroyNotify)dtb_destroy_node);
    }

    g_free(node);
}

void dtb_remove_node(DTBNode *parent, DTBNode *node)
{
    GList *iter;

    g_assert_nonnull(parent);
    g_assert_nonnull(node);

    for (iter = parent->children; iter != NULL; iter = iter->next) {
        if (node == iter->data) {
            dtb_destroy_node(node);
            parent->children = g_list_delete_link(parent->children, iter);
            return;
        }
    }

    g_assert_not_reached();
}

bool dtb_remove_node_named(DTBNode *parent, const char *name)
{
    DTBNode *node;

    g_assert_nonnull(parent);
    g_assert_nonnull(name);

    node = dtb_get_node(parent, name);

    if (node == NULL) {
        return false;
    }

    dtb_remove_node(parent, node);
    return true;
}

bool dtb_remove_prop_named(DTBNode *node, const char *name)
{
    g_assert_nonnull(node);
    g_assert_nonnull(name);

    return g_hash_table_remove(node->props, name);
}

DTBProp *dtb_set_prop(DTBNode *node, const char *name, const uint32_t size,
                      const void *val)
{
    DTBProp *prop;

    g_assert_nonnull(node);
    g_assert_nonnull(name);

    if (val == NULL) {
        g_assert_cmpuint(size, ==, 0);
    } else {
        g_assert_cmpuint(size, !=, 0);
    }

    g_assert_cmpint(strnlen(name, DT_PROP_NAME_LEN), <, DT_PROP_NAME_LEN);

    prop = dtb_find_prop(node, name);

    if (prop == NULL) {
        prop = g_new0(DTBProp, 1);
        g_hash_table_insert(node->props, g_strdup(name), prop);
    } else {
        g_free(prop->data);
        memset(prop, 0, sizeof(DTBProp));
    }

    prop->length = size;

    if (val != NULL) {
        prop->data = g_malloc0(size);
        memcpy(prop->data, val, size);
    }

    return prop;
}

DTBProp *dtb_set_prop_null(DTBNode *node, const char *name)
{
    return dtb_set_prop(node, name, 0, NULL);
}

DTBProp *dtb_set_prop_u32(DTBNode *node, const char *name, const uint32_t val)
{
    return dtb_set_prop(node, name, sizeof(val), &val);
}

DTBProp *dtb_set_prop_u64(DTBNode *node, const char *name, const uint64_t val)
{
    return dtb_set_prop(node, name, sizeof(val), &val);
}

DTBProp *dtb_set_prop_hwaddr(DTBNode *node, const char *name, const hwaddr val)
{
    return dtb_set_prop(node, name, sizeof(val), &val);
}

static uint32_t dtb_get_placeholder_size(DTBProp *prop, const char *name)
{
    char *string;
    char *next;
    const char *token;
    uint32_t len;

    g_assert_nonnull(prop->data);
    g_assert_cmphex(prop->length, >, 0);

    next = string = g_new0(char, prop->length);
    memcpy(next, prop->data, prop->length);

    while ((token = qemu_strsep(&next, ",")) != NULL) {
        if (*token == '\0') {
            continue;
        }

        if (strncmp(token, "macaddr/", 8) == 0) {
            g_free(string);
            return 6;
        }

        if (strncmp(token, "syscfg/", 7) == 0) {
            len = g_ascii_strtoull(token + 8 + 4, NULL, 0);
            if (len == 0) {
                continue;
            }
            g_free(string);
            return len;
        }

        if (strncmp(token, "zeroes/", 7) == 0) {
            len = g_ascii_strtoull(token + 7, NULL, 0);
            g_free(string);
            return len;
        }
    }

    g_free(string);
    return 0;
}

static void dtb_serialise_node(DTBNode *node, uint8_t **buf)
{
    uint8_t *prop_count_ptr;
    uint32_t prop_count;
    GHashTableIter ht_iter;
    gpointer key;
    DTBProp *prop;
    uint32_t placeholder_size;

    g_assert_nonnull(node);
    g_assert_nonnull(buf);
    g_assert_nonnull(*buf);

    prop_count = g_hash_table_size(node->props);
    prop_count_ptr = *buf;
    *buf += sizeof(uint32_t);

    stl_le_p(*buf, g_list_length(node->children));
    *buf += sizeof(uint32_t);

    g_hash_table_iter_init(&ht_iter, node->props);
    while (g_hash_table_iter_next(&ht_iter, &key, (gpointer *)&prop)) {
        g_assert_nonnull(prop);

        // TODO: put a system to register things like syscfg values.
        // who's going to have to do it? spoiler: it will be me, Visual, once
        // again.
        if (prop->placeholder) {
            placeholder_size = dtb_get_placeholder_size(prop, key);
            if (placeholder_size == 0) {
                fprintf(stderr, "warning: removing prop `%s`\n", (char *)key);
                prop_count -= 1;
                continue;
            }
            fprintf(stderr, "warning: expanding prop `%s` to default value\n",
                    (char *)key);
            strncpy((char *)*buf, key, DT_PROP_NAME_LEN);
            *buf += DT_PROP_NAME_LEN;
            stl_le_p(*buf, placeholder_size);
            *buf += sizeof(uint32_t);
            *buf += ROUND_UP(placeholder_size, 4);
        } else {
            strncpy((char *)*buf, key, DT_PROP_NAME_LEN);
            *buf += DT_PROP_NAME_LEN;
            stl_le_p(*buf, prop->length);
            *buf += sizeof(uint32_t);

            if (prop->length == 0) {
                g_assert_null(prop->data);
            } else {
                g_assert_nonnull(prop->data);
                memcpy(*buf, prop->data, prop->length);
                *buf += ROUND_UP(prop->length, 4);
            }
        }
    }
    stl_le_p(prop_count_ptr, prop_count);

    g_list_foreach(node->children, (GFunc)dtb_serialise_node, buf);
}

void dtb_serialise(uint8_t *buf, DTBNode *root)
{
    g_assert_nonnull(buf);
    g_assert_nonnull(root);

    dtb_serialise_node(root, &buf);
}

static uint64_t dtb_get_serialised_prop_size(DTBProp *prop, const char *name)
{
    uint32_t placeholder_size;

    g_assert_nonnull(prop);

    if (prop->placeholder) {
        placeholder_size = dtb_get_placeholder_size(prop, name);
        if (placeholder_size == 0) {
            return 0;
        }
        return DT_PROP_NAME_LEN + sizeof(prop->length) +
               ROUND_UP(placeholder_size, 4);
    }
    return DT_PROP_NAME_LEN + sizeof(prop->length) + ROUND_UP(prop->length, 4);
}

uint64_t dtb_get_serialised_node_size(DTBNode *node)
{
    g_assert_nonnull(node);

    GHashTableIter ht_iter;
    gpointer key;
    gpointer value;
    uint64_t size;
    GList *iter;

    size = sizeof(uint32_t) + sizeof(uint32_t);

    g_hash_table_iter_init(&ht_iter, node->props);
    while (g_hash_table_iter_next(&ht_iter, &key, &value)) {
        g_assert_nonnull(value);
        size += dtb_get_serialised_prop_size(value, key);
    }

    for (iter = node->children; iter != NULL; iter = iter->next) {
        g_assert_nonnull(iter->data);
        size += dtb_get_serialised_node_size(iter->data);
    }

    return size;
}

DTBProp *dtb_find_prop(DTBNode *node, const char *name)
{
    return g_hash_table_lookup(node->props, name);
}

DTBNode *dtb_get_node(DTBNode *node, const char *path)
{
    GList *iter;
    DTBProp *prop;
    DTBNode *child;
    char *next;
    char *string;
    const char *token;
    bool found;

    g_assert_nonnull(node);
    g_assert_nonnull(path);

    next = string = g_strdup(path);

    while (node != NULL && ((token = qemu_strsep(&next, "/")))) {
        if (*token == '\0') {
            continue;
        }

        found = false;

        for (iter = node->children; iter; iter = iter->next) {
            child = (DTBNode *)iter->data;

            g_assert_nonnull(child);

            prop = dtb_find_prop(child, "name");

            if (prop == NULL) {
                continue;
            }

            if (strncmp((const char *)prop->data, token, prop->length) == 0) {
                node = child;
                found = true;
                break;
            }
        }

        if (!found) {
            g_free(string);
            return NULL;
        }
    }

    g_free(string);
    return node;
}
