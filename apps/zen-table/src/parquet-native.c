/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "parquet-native.h"
#include <carquet/carquet.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define CELL_BUDGET (16u * 1024 * 1024)
#define PAGE_BUDGET CELL_BUDGET
#define ARRAY_BUDGET (64u * 1024 * 1024)
#define NESTED_INPUT_BUDGET (16u * 1024 * 1024)
#define FLAT_RESEEK_BYTES (32u * 1024 * 1024)
#define MAX_DEPTH 64

typedef struct Format {
    const struct ArrowSchema *schema;
    const carquet_schema_node_t *leaf;
    struct Format **children;
    char *type;
} Format;
struct ZtParquet {
    carquet_reader_t *reader;
    const carquet_schema_t *schema;
    struct ArrowSchema arrow_schema;
    Format *format;
    GArray *group_starts;
    guint rows, columns;
    gboolean nested, has_binary_leaves;
    guint64 flat_window_bytes;
    GMutex lock;
    gint cancelled;
    carquet_batch_reader_t *batch_reader;
    struct ArrowArray array;
    gint64 array_start, next_row;
    gint cached_group;
};

static gboolean bad(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE;
}
static gboolean carquet_error(GError **error, const carquet_error_t *detail, const char *context) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: %s", context,
                detail && detail->message[0] ? detail->message : "The Parquet data is invalid or unsupported.");
    return FALSE;
}
static gboolean cancelled(ZtParquet *p, GCancellable *cancel, GError **error) {
    if (g_atomic_int_get(&p->cancelled)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Table loading was cancelled."); return TRUE;
    }
    return g_cancellable_set_error_if_cancelled(cancel, error);
}
static void release_array(ZtParquet *p) {
    if (p->array.release) p->array.release(&p->array);
    memset(&p->array, 0, sizeof p->array); p->array_start = -1; p->cached_group = -1;
}
static void format_free(Format *format) {
    if (!format) return;
    for (gint64 i = 0; format->children && i < format->schema->n_children; i++) format_free(format->children[i]);
    g_free(format->children); g_free(format->type); g_free(format);
}
void zt_parquet_free(ZtParquet *p) {
    if (!p) return;
    release_array(p);
    if (p->batch_reader) carquet_batch_reader_free(p->batch_reader);
    format_free(p->format);
    if (p->arrow_schema.release) p->arrow_schema.release(&p->arrow_schema);
    if (p->reader) carquet_reader_close(p->reader);
    g_clear_pointer(&p->group_starts, g_array_unref); g_mutex_clear(&p->lock); g_free(p);
}
void zt_parquet_cancel(ZtParquet *p) { if (p) g_atomic_int_set(&p->cancelled, 1); }
guint zt_parquet_rows(ZtParquet *p) { return p->rows; }
guint zt_parquet_columns(ZtParquet *p) { return p->columns; }
const char *zt_parquet_header(ZtParquet *p, guint i) { return p->arrow_schema.children[i]->name; }
const char *zt_parquet_type(ZtParquet *p, guint i) { return p->format->children[i]->type; }

static char *leaf_type(const carquet_schema_node_t *node) {
    const carquet_logical_type_t *logical = carquet_schema_node_logical_type(node);
    if (logical) switch (logical->id) {
    case CARQUET_LOGICAL_STRING: case CARQUET_LOGICAL_ENUM: return g_strdup("string");
    case CARQUET_LOGICAL_JSON: return g_strdup("json");
    case CARQUET_LOGICAL_DECIMAL: return g_strdup_printf("decimal(%d, %d)", logical->params.decimal.precision, logical->params.decimal.scale);
    case CARQUET_LOGICAL_DATE: return g_strdup("date");
    case CARQUET_LOGICAL_TIME: return g_strdup("time");
    case CARQUET_LOGICAL_TIMESTAMP: return g_strdup(logical->params.timestamp.is_adjusted_to_utc ? "timestamp (UTC)" : "timestamp");
    case CARQUET_LOGICAL_INTEGER: return g_strdup_printf("%sint%d", logical->params.integer.is_signed ? "" : "u", logical->params.integer.bit_width);
    case CARQUET_LOGICAL_NULL: return g_strdup("null");
    case CARQUET_LOGICAL_UUID: return g_strdup("uuid");
    case CARQUET_LOGICAL_FLOAT16: return g_strdup("float16");
    case CARQUET_LOGICAL_INTERVAL: return g_strdup("interval");
    default: break;
    }
    switch (carquet_schema_node_physical_type(node)) {
    case CARQUET_PHYSICAL_BOOLEAN: return g_strdup("bool");
    case CARQUET_PHYSICAL_INT32: return g_strdup("int32");
    case CARQUET_PHYSICAL_INT64: return g_strdup("int64");
    case CARQUET_PHYSICAL_INT96: return g_strdup("timestamp (INT96)");
    case CARQUET_PHYSICAL_FLOAT: return g_strdup("float32");
    case CARQUET_PHYSICAL_DOUBLE: return g_strdup("float64");
    case CARQUET_PHYSICAL_FIXED_LEN_BYTE_ARRAY: return g_strdup_printf("binary[%d]", carquet_schema_node_type_length(node));
    default: return g_strdup("binary");
    }
}
static Format *format_new(const struct ArrowSchema *schema, GPtrArray *leaves, guint *leaf_index, guint depth, GError **error) {
    if (depth > MAX_DEPTH || schema->n_children < 0 || schema->n_children > ZT_MAX_COLUMNS) { bad(error, "This Parquet schema exceeds the nesting or column limits."); return NULL; }
    Format *format = g_new0(Format, 1); format->schema = schema;
    if (!schema->n_children && schema->format[0] != '+') {
        if (*leaf_index >= leaves->len) { bad(error, "The Parquet schema does not match its columns."); format_free(format); return NULL; }
        format->leaf = g_ptr_array_index(leaves, (*leaf_index)++); format->type = leaf_type(format->leaf); return format;
    }
    format->children = g_new0(Format *, schema->n_children);
    for (gint64 i = 0; i < schema->n_children; i++) {
        format->children[i] = format_new(schema->children[i], leaves, leaf_index, depth + 1, error);
        if (!format->children[i]) { format_free(format); return NULL; }
    }
    const char *kind = !strcmp(schema->format, "+s") ? "struct" : !strcmp(schema->format, "+m") ? "map" : "list";
    g_autoptr(GString) type = g_string_new(kind); g_string_append_c(type, '<');
    for (gint64 i = 0; i < schema->n_children; i++) {
        if (i) g_string_append(type, ", ");
        if (!strcmp(kind, "struct")) { g_string_append(type, schema->children[i]->name); g_string_append(type, ": "); }
        g_string_append(type, format->children[i]->type);
        if (type->len > 2048) { g_string_truncate(type, 2048); g_string_append(type, "…"); break; }
    }
    g_string_append_c(type, '>'); format->type = g_string_free(g_steal_pointer(&type), FALSE); return format;
}
ZtParquet *zt_parquet_open(const char *path, GCancellable *cancel, GError **error) {
    g_autoptr(GPtrArray) leaves = NULL;
    ZtParquet *p = g_new0(ZtParquet, 1); g_mutex_init(&p->lock); p->array_start = p->next_row = -1; p->cached_group = -1;
    if (cancelled(p, cancel, error)) goto failure;
    carquet_reader_options_t options; carquet_reader_options_init(&options); options.num_threads = 1; options.use_mmap = FALSE;
    carquet_error_t detail = {0}; p->reader = carquet_reader_open(path, &options, &detail);
    if (!p->reader) { carquet_error(error, &detail, "Cannot open Parquet"); goto failure; }
    gint64 rows = carquet_reader_num_rows(p->reader);
    if (rows < 0 || rows > G_MAXUINT) { bad(error, "This Parquet table exceeds the row limit."); goto failure; }
    p->rows = rows; p->schema = carquet_reader_schema(p->reader);
    if (carquet_arrow_export_schema(p->schema, &p->arrow_schema, &detail) != CARQUET_OK) {
        carquet_error(error, &detail, "Cannot read the Parquet schema"); goto failure;
    }
    if (p->arrow_schema.n_children <= 0 || p->arrow_schema.n_children > ZT_MAX_COLUMNS) { bad(error, "This Parquet table must have between 1 and 512 columns."); goto failure; }
    p->columns = p->arrow_schema.n_children;
    leaves = g_ptr_array_new();
    gint elements = carquet_schema_num_elements(p->schema);
    for (gint i = 0; i < elements; i++) {
        const carquet_schema_node_t *node = carquet_schema_get_element(p->schema, i);
        if (node && carquet_schema_node_is_leaf(node)) {
            g_ptr_array_add(leaves, (gpointer)node);
            if (carquet_schema_node_physical_type(node) == CARQUET_PHYSICAL_BYTE_ARRAY) p->has_binary_leaves = TRUE;
        }
    }
    guint leaf_index = 0; p->format = format_new(&p->arrow_schema, leaves, &leaf_index, 0, error);
    if (!p->format) goto failure;
    if (leaf_index != leaves->len) { bad(error, "The Parquet schema does not match its columns."); goto failure; }
    for (guint i = 0; i < p->columns; i++) if (p->arrow_schema.children[i]->n_children || p->arrow_schema.children[i]->format[0] == '+') p->nested = TRUE;
    p->group_starts = g_array_new(FALSE, FALSE, sizeof(guint64));
    guint64 position = 0;
    for (gint i = 0; i < carquet_reader_num_row_groups(p->reader); i++) {
        carquet_row_group_metadata_t metadata;
        if (carquet_reader_row_group_metadata(p->reader, i, &metadata) != CARQUET_OK || metadata.num_rows < 0 || (guint64)metadata.num_rows > p->rows - position) {
            bad(error, "The Parquet row-group counts are invalid."); goto failure;
        }
        g_array_append_val(p->group_starts, position); position += metadata.num_rows;
    }
    g_array_append_val(p->group_starts, position);
    if (position != p->rows) { bad(error, "The Parquet row count does not match its row groups."); goto failure; }
    if (!p->nested) {
        carquet_batch_reader_config_t config; carquet_batch_reader_config_init(&config);
        config.batch_size = ZT_PAGE_SIZE; config.num_threads = 1; config.use_mmap = FALSE; config.preserve_dictionaries = FALSE;
        p->batch_reader = carquet_batch_reader_create(p->reader, &config, &detail);
        if (!p->batch_reader) { carquet_error(error, &detail, "Cannot create the Parquet reader"); goto failure; }
    }
    if (cancelled(p, cancel, error)) goto failure;
    return p;
failure:
    zt_parquet_free(p); return NULL;
}

/* Arrow uses a small, public C ABI, not an Arrow runtime dependency. */
static gboolean append(GString *text, const char *value, gssize length, GError **error) {
    gsize n = length < 0 ? strlen(value) : (gsize)length;
    if (n > CELL_BUDGET || text->len > CELL_BUDGET - n) return bad(error, "A cell exceeds the 16 MiB display budget.");
    g_string_append_len(text, value, n); return TRUE;
}
static gboolean quoted(GString *text, const char *value, gsize length, GError **error) {
    if (!append(text, "\"", 1, error)) return FALSE;
    for (gsize i = 0; i < length; i++) {
        char escaped[7]; const char *part = value + i; gsize size = 1;
        switch ((guchar)value[i]) {
        case '\"': part = "\\\""; size = 2; break;
        case '\\': part = "\\\\"; size = 2; break;
        case '\n': part = "\\n"; size = 2; break;
        case '\r': part = "\\r"; size = 2; break;
        case '\t': part = "\\t"; size = 2; break;
        default:
            if ((guchar)value[i] < 0x20) { g_snprintf(escaped, sizeof escaped, "\\u%04x", (guchar)value[i]); part = escaped; size = 6; }
        }
        if (!append(text, part, size, error)) return FALSE;
    }
    return append(text, "\"", 1, error);
}
static gboolean text_value(GString *out, const char *data, gsize length, gboolean json, GError **error) {
    if (length > CELL_BUDGET) return bad(error, "A cell exceeds the 16 MiB display budget.");
    g_autofree char *valid = g_utf8_make_valid(data, length);
    if (json) return quoted(out, valid, strlen(valid), error);
    return append(out, valid, -1, error);
}
static gboolean bytes_value(GString *out, const guint8 *data, gsize size, gboolean json, GError **error) {
    if (size > (CELL_BUDGET - 4) / 2) return bad(error, "A binary cell exceeds the 16 MiB display budget.");
    if (!append(out, json ? "\"0x" : "0x", json ? 3 : 2, error)) return FALSE;
    static const char hex[] = "0123456789abcdef";
    for (gsize i = 0; i < size; i++) { char pair[2] = {hex[data[i] >> 4], hex[data[i] & 15]}; if (!append(out, pair, 2, error)) return FALSE; }
    return !json || append(out, "\"", 1, error);
}
static char *decimal_text(const guint8 *bytes, gsize length, gboolean negative, gint scale, GError **error) {
    if (length > 32 || scale < 0 || scale > 76) { bad(error, "This decimal exceeds the supported decimal256 precision or scale."); return NULL; }
    guint8 magnitude[32] = {0}, digits[80] = {0}; guint ndigits = 1;
    memcpy(magnitude, bytes, length);
    if (negative) {
        guint carry = 1;
        for (gint i = (gint)length - 1; i >= 0; i--) { guint value = (magnitude[i] ^ 255u) + carry; magnitude[i] = value & 255; carry = value >> 8; }
    }
    for (gsize i = 0; i < length; i++) {
        guint carry = magnitude[i];
        for (guint j = 0; j < ndigits; j++) { guint value = digits[j] * 256u + carry; digits[j] = value % 10; carry = value / 10; }
        while (carry) { digits[ndigits++] = carry % 10; carry /= 10; }
    }
    GString *result = g_string_new(NULL);
    if (negative && !(ndigits == 1 && digits[0] == 0)) g_string_append_c(result, '-');
    if (scale >= (gint)ndigits) {
        g_string_append(result, "0.");
        for (gint i = scale - ndigits; i > 0; i--) g_string_append_c(result, '0');
    }
    for (gint i = (gint)ndigits - 1; i >= 0; i--) {
        g_string_append_c(result, '0' + digits[i]);
        if (scale && i == scale && scale < (gint)ndigits) g_string_append_c(result, '.');
    }
    return g_string_free(result, FALSE);
}
static gboolean scalar_text(GString *out, const char *value, gboolean json, GError **error) {
    return json ? quoted(out, value, strlen(value), error) : append(out, value, -1, error);
}
static gboolean temporal_parts(GString *out, gint64 seconds, gint64 fraction, gint unit, gboolean time_only, gboolean utc, gboolean json, GError **error) {
    g_autofree char *base = NULL;
    if (time_only) {
        if (seconds < 0 || seconds >= 86400) return bad(error, "A Parquet time is outside the valid day range.");
        base = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 3600, seconds / 60 % 60, seconds % 60);
    } else {
        g_autoptr(GDateTime) date = g_date_time_new_from_unix_utc(seconds);
        if (!date) return bad(error, "A timestamp is outside the supported calendar range (years 1–9999).");
        base = g_date_time_format(date, "%Y-%m-%d %H:%M:%S");
    }
    g_autoptr(GString) text = g_string_new(base);
    if (fraction) g_string_append_printf(text, ".%0*" G_GINT64_FORMAT, unit == CARQUET_TIME_UNIT_MILLIS ? 3 : unit == CARQUET_TIME_UNIT_MICROS ? 6 : 9, fraction);
    if (utc && !time_only) g_string_append(text, "+00:00");
    return scalar_text(out, text->str, json, error);
}
static gboolean temporal(GString *out, gint64 value, gint unit, gboolean time_only, gboolean utc, gboolean json, GError **error) {
    gint64 divisor = unit == CARQUET_TIME_UNIT_MILLIS ? 1000 : unit == CARQUET_TIME_UNIT_MICROS ? 1000000 : 1000000000;
    gint64 seconds = value / divisor, fraction = value % divisor;
    if (fraction < 0) { seconds--; fraction += divisor; }
    return temporal_parts(out, seconds, fraction, unit, time_only, utc, json, error);
}
static gint64 signed_integer(const char *format, const void *data, gint64 index) {
    switch (*format) {
    case 'c': return ((const gint8 *)data)[index]; case 'C': return ((const guint8 *)data)[index];
    case 's': return ((const gint16 *)data)[index]; case 'S': return ((const guint16 *)data)[index];
    case 'i': return ((const gint32 *)data)[index]; case 'I': return ((const guint32 *)data)[index];
    case 'l': return ((const gint64 *)data)[index]; default: return ((const gint64 *)data)[index];
    }
}
static double half_float(guint16 value) {
    gint exponent = (value >> 10) & 31, fraction = value & 1023;
    double number = exponent == 31 ? (fraction ? NAN : INFINITY) : exponent ? ldexp(1. + fraction / 1024., exponent - 15) : ldexp(fraction, -24);
    return value & 0x8000 ? -number : number;
}
static gboolean cell(GString *out, const Format *format, const struct ArrowArray *array, gint64 row, gboolean json, guint depth, GError **error) {
    if (depth > MAX_DEPTH || row < 0 || row >= array->length || array->offset < 0 || array->offset > G_MAXINT64 - row)
        return bad(error, "Invalid nested Parquet array bounds.");
    gint64 index = array->offset + row;
    const char *kind = format->schema->format;
    if (!array->buffers || array->n_buffers < 1) return bad(error, "Invalid Parquet array buffers.");
    const guint8 *validity = array->buffers[0];
    if (validity && !(validity[index / 8] & (1u << (index % 8)))) return !json || append(out, "null", 4, error);
    if (*kind == '+') {
        if (array->n_children != format->schema->n_children) return bad(error, "Invalid nested Parquet child count.");
        if (!strcmp(kind, "+s")) {
            if (!append(out, "{", 1, error)) return FALSE;
            for (gint64 i = 0; i < array->n_children; i++) {
                if (i && !append(out, ", ", 2, error)) return FALSE;
                const char *name = format->children[i]->schema->name;
                if (!quoted(out, name, strlen(name), error) || !append(out, ": ", 2, error) ||
                    !cell(out, format->children[i], array->children[i], index, TRUE, depth + 1, error)) return FALSE;
            }
            return append(out, "}", 1, error);
        }
        if ((!strcmp(kind, "+l") || !strcmp(kind, "+L") || !strcmp(kind, "+m")) && array->n_children == 1 && array->n_buffers >= 2 && array->buffers[1]) {
            gint64 first = !strcmp(kind, "+L") ? ((const gint64 *)array->buffers[1])[index] : ((const gint32 *)array->buffers[1])[index];
            gint64 end = !strcmp(kind, "+L") ? ((const gint64 *)array->buffers[1])[index + 1] : ((const gint32 *)array->buffers[1])[index + 1];
            if (first < 0 || end < first || end > array->children[0]->length) return bad(error, "Invalid Parquet list offsets.");
            if (!append(out, "[", 1, error)) return FALSE;
            for (gint64 i = first; i < end; i++) {
                if (i != first && !append(out, ", ", 2, error)) return FALSE;
                if (!strcmp(kind, "+m")) {
                    const Format *entries = format->children[0]; const struct ArrowArray *values = array->children[0];
                    if (entries->schema->n_children != 2 || values->n_children != 2) return bad(error, "Invalid Parquet map entries.");
                    if (!append(out, "[", 1, error) || !cell(out, entries->children[0], values->children[0], i, TRUE, depth + 1, error) ||
                        !append(out, ", ", 2, error) || !cell(out, entries->children[1], values->children[1], i, TRUE, depth + 1, error) || !append(out, "]", 1, error)) return FALSE;
                } else if (!cell(out, format->children[0], array->children[0], i, TRUE, depth + 1, error)) return FALSE;
            }
            return append(out, "]", 1, error);
        }
        return bad(error, "This nested Parquet array layout is not supported.");
    }
    if (!format->leaf || array->n_buffers < 2 || !array->buffers[1]) return bad(error, "Invalid Parquet value buffers.");
    const carquet_logical_type_t *logical = carquet_schema_node_logical_type(format->leaf);
    carquet_physical_type_t physical = carquet_schema_node_physical_type(format->leaf);
    const void *data = array->buffers[1]; const guint8 *bytes = NULL; gint64 length = 0;
    if (*kind == 'u' || *kind == 'z' || *kind == 'U' || *kind == 'Z') {
        if (array->n_buffers < 3) return bad(error, "Invalid Parquet binary buffers.");
        gint64 start = (*kind == 'U' || *kind == 'Z') ? ((const gint64 *)data)[index] : ((const gint32 *)data)[index];
        gint64 end = (*kind == 'U' || *kind == 'Z') ? ((const gint64 *)data)[index + 1] : ((const gint32 *)data)[index + 1];
        if (start < 0 || end < start || end - start > CELL_BUDGET || (!array->buffers[2] && end)) return bad(error, "A binary cell exceeds the display budget or has invalid offsets.");
        length = end - start; bytes = length ? (const guint8 *)array->buffers[2] + start : (const guint8 *)"";
    } else if (!strncmp(kind, "w:", 2)) {
        length = g_ascii_strtoll(kind + 2, NULL, 10);
        if (length <= 0 || length > CELL_BUDGET || (guint64)index > G_MAXSIZE / length) return bad(error, "Invalid fixed-size Parquet binary width.");
        bytes = (const guint8 *)data + index * length;
    }
    if (logical && logical->id == CARQUET_LOGICAL_NULL) return !json || append(out, "null", 4, error);
    if (logical && logical->id == CARQUET_LOGICAL_DECIMAL) {
        guint8 integer[8]; gboolean negative;
        if (bytes) negative = length && (bytes[0] & 128);
        else {
            gint64 number = physical == CARQUET_PHYSICAL_INT32 ? ((const gint32 *)data)[index] : ((const gint64 *)data)[index];
            negative = number < 0; guint64 bits = (guint64)number;
            for (guint i = 0; i < 8; i++) integer[7 - i] = bits >> (8 * i);
            bytes = integer; length = 8;
        }
        g_autofree char *decimal = decimal_text(bytes, length, negative, logical->params.decimal.scale, error);
        return decimal && scalar_text(out, decimal, json, error);
    }
    if (logical && logical->id == CARQUET_LOGICAL_DATE) {
        gint64 days = ((const gint32 *)data)[index]; g_autoptr(GDateTime) date = g_date_time_new_from_unix_utc(days * 86400);
        if (!date) return bad(error, "A date is outside the supported calendar range (years 1–9999).");
        g_autofree char *value = g_date_time_format(date, "%Y-%m-%d"); return scalar_text(out, value, json, error);
    }
    if (logical && (logical->id == CARQUET_LOGICAL_TIME || logical->id == CARQUET_LOGICAL_TIMESTAMP)) {
        gint64 number = physical == CARQUET_PHYSICAL_INT32 ? ((const gint32 *)data)[index] : ((const gint64 *)data)[index];
        gboolean time = logical->id == CARQUET_LOGICAL_TIME;
        return temporal(out, number, time ? logical->params.time.unit : logical->params.timestamp.unit, time,
                        !time && logical->params.timestamp.is_adjusted_to_utc, json, error);
    }
    if (physical == CARQUET_PHYSICAL_INT96 && bytes && length == 12) {
        guint64 nanos; guint32 julian; memcpy(&nanos, bytes, 8); memcpy(&julian, bytes + 8, 4);
        nanos = GUINT64_FROM_LE(nanos); julian = GUINT32_FROM_LE(julian);
        gint64 days = (gint64)julian - 2440588;
        if (nanos >= G_GUINT64_CONSTANT(86400000000000)) return bad(error, "An INT96 timestamp contains an invalid time of day.");
        /* INT96 has a much wider calendar range than int64 nanoseconds. Keep
         * whole seconds and the fractional nanoseconds separate; combining
         * them wraps valid dates beyond the 1677–2262 nanosecond range. */
        return temporal_parts(out, days * 86400 + nanos / 1000000000, nanos % 1000000000,
                              CARQUET_TIME_UNIT_NANOS, FALSE, FALSE, json, error);
    }
    if (logical && logical->id == CARQUET_LOGICAL_UUID && bytes && length == 16) {
        g_autofree char *uuid = g_strdup_printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
        return scalar_text(out, uuid, json, error);
    }
    if (bytes) return *kind == 'u' || *kind == 'U' ? text_value(out, (const char *)bytes, length, json, error) : bytes_value(out, bytes, length, json, error);
    char number[G_ASCII_DTOSTR_BUF_SIZE];
    if (*kind == 'b') return append(out, ((const guint8 *)data)[index / 8] & (1u << (index % 8)) ? "true" : "false", -1, error);
    if (*kind == 'f' || *kind == 'g' || *kind == 'e') {
        double value = *kind == 'f' ? ((const float *)data)[index] : *kind == 'g' ? ((const double *)data)[index] : half_float(((const guint16 *)data)[index]);
        if (!isfinite(value)) return scalar_text(out, isnan(value) ? "NaN" : value < 0 ? "-Infinity" : "Infinity", json, error);
        g_ascii_formatd(number, sizeof number, *kind == 'f' ? "%.9g" : "%.17g", value);
    } else if (*kind == 'L') g_snprintf(number, sizeof number, "%" G_GUINT64_FORMAT, ((const guint64 *)data)[index]);
    else g_snprintf(number, sizeof number, "%" G_GINT64_FORMAT, signed_integer(kind, data, index));
    return append(out, number, -1, error);
}

/* Estimate actual exported Arrow buffers before retaining a row group. These
 * are post-decode guards: the codec may use temporary memory while decoding. */
static gboolean array_bytes(const struct ArrowSchema *schema, const struct ArrowArray *array, guint depth, guint64 *total) {
    if (depth > MAX_DEPTH || array->length < 0 || array->offset < 0 || array->length > G_MAXINT64 - array->offset) return FALSE;
    guint64 n = array->length + array->offset, bytes = n / 8 + 1;
    const char *kind = schema->format;
    if (*kind == '+') {
        if (!strcmp(kind, "+l") || !strcmp(kind, "+m")) bytes += (n + 1) * 4;
        else if (!strcmp(kind, "+L")) bytes += (n + 1) * 8;
    } else if (*kind == 'u' || *kind == 'z' || *kind == 'U' || *kind == 'Z') {
        if (array->n_buffers < 3 || !array->buffers[1]) return FALSE;
        gboolean wide = *kind == 'U' || *kind == 'Z';
        gint64 size = wide ? ((const gint64 *)array->buffers[1])[n] : ((const gint32 *)array->buffers[1])[n];
        if (size < 0 || size > ARRAY_BUDGET) return FALSE;
        bytes += (n + 1) * (wide ? 8 : 4) + size;
    } else {
        guint64 stride = *kind == 'b' ? 0 : strchr("cC", *kind) ? 1 : strchr("sSe", *kind) ? 2 : strchr("iIf", *kind) || !strcmp(kind, "tdD") || !strcmp(kind, "ttm") ? 4 : 8;
        if (!strncmp(kind, "w:", 2)) stride = g_ascii_strtoull(kind + 2, NULL, 10);
        if (stride > ARRAY_BUDGET || n > ARRAY_BUDGET / MAX(stride, 1)) return FALSE;
        bytes += stride ? n * stride : n / 8 + 1;
    }
    if (bytes > ARRAY_BUDGET || *total > ARRAY_BUDGET - bytes) return FALSE;
    *total += bytes;
    if (array->n_children != schema->n_children) return FALSE;
    for (gint64 i = 0; i < array->n_children; i++) if (!array_bytes(schema->children[i], array->children[i], depth + 1, total)) return FALSE;
    return TRUE;
}
static guint group_for(ZtParquet *p, guint position) {
    guint low = 0, high = p->group_starts->len - 1;
    while (low < high) { guint middle = low + (high - low) / 2; if (g_array_index(p->group_starts, guint64, middle + 1) <= position) low = middle + 1; else high = middle; }
    return low;
}
static gboolean nested_budget(ZtParquet *p, guint group, GError **error) {
    carquet_row_group_metadata_t metadata;
    if (carquet_reader_row_group_metadata(p->reader, group, &metadata) != CARQUET_OK || metadata.total_byte_size < 0)
        return bad(error, "Invalid Parquet row-group metadata.");
    if (metadata.total_byte_size > NESTED_INPUT_BUDGET) return bad(error, "This nested Parquet row group exceeds the 16 MiB input budget. Rewrite it with smaller row groups to view it.");
    guint64 estimate = (guint64)metadata.total_byte_size * 2;
    for (gint i = 0; i < carquet_reader_num_columns(p->reader); i++) {
        carquet_column_chunk_metadata_t column;
        if (carquet_reader_column_chunk_metadata(p->reader, group, i, &column) != CARQUET_OK || column.num_values < 0)
            return bad(error, "Invalid Parquet column metadata.");
        /* Reassembly holds values + definition/repetition levels and its final
         * arrays. Count each physical slot even if an RLE run encoded it tiny. */
        guint stride = column.type == CARQUET_PHYSICAL_BYTE_ARRAY ? 40 : column.type == CARQUET_PHYSICAL_INT96 ? 28 : 24;
        if ((guint64)column.num_values > ARRAY_BUDGET / stride || estimate > ARRAY_BUDGET - (guint64)column.num_values * stride)
            return bad(error, "This nested Parquet row group exceeds the 64 MiB decode budget. Rewrite it with smaller row groups to view it.");
        estimate += (guint64)column.num_values * stride;
    }
    return TRUE;
}
static gboolean load_array(ZtParquet *p, guint position, GCancellable *cancel, GError **error) {
    if (p->array.release && position >= p->array_start && (guint64)(position - p->array_start) < (guint64)p->array.length) return TRUE;
    if (cancelled(p, cancel, error)) return FALSE;
    release_array(p); carquet_error_t detail = {0};
    if (p->nested) {
        guint group = group_for(p, position);
        if (!nested_budget(p, group, error)) return FALSE;
        if (carquet_reader_read_arrow(p->reader, group, NULL, &p->array, &detail) != CARQUET_OK)
            return carquet_error(error, &detail, "Cannot decode this nested Parquet row group");
        p->array_start = g_array_index(p->group_starts, guint64, group); p->cached_group = group;
    } else {
        if (p->next_row != position || (p->has_binary_leaves && p->flat_window_bytes >= FLAT_RESEEK_BYTES)) {
            /* Carquet 0.7.3 retains decoded variable-width pages until a row
             * group reset. Reposition periodically to release old pages during
             * long scans; column_skip skips whole pages without decoding, and
             * an offset index accelerates the seek when the file supplies one.
             * This is a working-set heuristic, not a hard decoder memory cap. */
            if (carquet_batch_reader_set_row_range(p->batch_reader, position, -1) != CARQUET_OK)
                return bad(error, "Cannot seek to the requested Parquet row.");
            p->flat_window_bytes = 0;
        }
        carquet_row_batch_t *batch = NULL;
        carquet_status_t status = carquet_batch_reader_next(p->batch_reader, &batch);
        if (status != CARQUET_OK || !batch) {
            p->next_row = -1;
            return carquet_error(error, carquet_batch_reader_last_error(p->batch_reader), "Cannot decode this Parquet page");
        }
        status = carquet_arrow_export_batch(batch, p->schema, NULL, &p->array, &detail); carquet_row_batch_free(batch);
        if (status != CARQUET_OK) { p->next_row = -1; return carquet_error(error, &detail, "Cannot read this Parquet page"); }
        p->array_start = position; p->next_row = (gint64)position + p->array.length;
    }
    guint64 size = 0;
    if (!array_bytes(&p->arrow_schema, &p->array, 0, &size)) { release_array(p); return bad(error, "This Parquet batch exceeds the 64 MiB decode budget."); }
    if (!p->array.length) { release_array(p); return bad(error, "The Parquet row count does not match its data."); }
    if (!p->nested) p->flat_window_bytes += size;
    return !cancelled(p, cancel, error);
}
ZtPage *zt_parquet_page(ZtParquet *p, guint start, guint count, GCancellable *cancel, GError **error) {
    ZtPage *page = g_new0(ZtPage, 1); page->start = start; page->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    if (start >= p->rows) return page;
    count = MIN(MIN(count, ZT_PAGE_SIZE), p->rows - start); guint64 bytes = 0; gboolean ok = TRUE;
    g_mutex_lock(&p->lock);
    for (guint row = 0; row < count && ok; row++) {
        if (cancelled(p, cancel, error) || !load_array(p, start + row, cancel, error)) { ok = FALSE; break; }
        char **values = g_new0(char *, p->columns + 1); g_ptr_array_add(page->rows, values);
        for (guint column = 0; column < p->columns; column++) {
            g_autoptr(GString) text = g_string_new(NULL);
            if (!cell(text, p->format->children[column], p->array.children[column], start + row - p->array_start, FALSE, 0, error)) { ok = FALSE; break; }
            bytes += text->len + sizeof(char *) + 1;
            if (bytes > PAGE_BUDGET) { bad(error, "This page exceeds the 16 MiB display budget. Split exceptionally large cells or columns before viewing."); ok = FALSE; break; }
            values[column] = g_string_free(g_steal_pointer(&text), FALSE);
        }
    }
    g_mutex_unlock(&p->lock);
    if (!ok) { zt_page_free(page); return NULL; }
    return page;
}
gint64 zt_parquet_find(ZtParquet *p, guint start, const char *text, GCancellable *cancel, GError **error) {
    g_autofree char *needle = g_utf8_casefold(text, -1); start = MIN(start, p->rows);
    guint64 ranges[4] = {start, p->rows, 0, start};
    for (guint segment = 0; segment < 2; segment++) {
        for (guint64 position = ranges[segment * 2]; position < ranges[segment * 2 + 1];) {
            guint count = MIN(ZT_PAGE_SIZE, ranges[segment * 2 + 1] - position);
            g_autoptr(ZtPage) page = zt_parquet_page(p, position, count, cancel, error); if (!page) return -1;
            for (guint i = 0; i < page->rows->len; i++) {
                char **values = g_ptr_array_index(page->rows, i);
                for (guint j = 0; values[j]; j++) { g_autofree char *folded = g_utf8_casefold(values[j], -1); if (strstr(folded, needle)) return position + i; }
            }
            if (!page->rows->len) break;
            position += page->rows->len;
        }
    }
    return -1;
}
