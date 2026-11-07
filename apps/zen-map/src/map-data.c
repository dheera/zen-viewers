/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "map-data.h"
#include <json-glib/json-glib.h>
#include <libxml/parser.h>
#include <math.h>
#include <string.h>

#define MAP_MAX_BYTES (64 * 1024 * 1024)
#define MAP_MAX_POINTS 2000000

static void shape_free(gpointer p) {
    MapShape *s = p; g_array_unref(s->points); g_free(s->name); g_free(s);
}
void map_data_free(MapData *d) {
    if (!d) return;
    g_ptr_array_unref(d->shapes); g_free(d);
}
double map_latitude_y(double lat) {
    double r = CLAMP(lat, -85.05112878, 85.05112878) * G_PI / 180.;
    return (1. - asinh(tan(r)) / G_PI) / 2.;
}
double map_y_latitude(double y) { return atan(sinh(G_PI * (1. - 2. * y))) * 180. / G_PI; }

typedef struct { MapData *data; GCancellable *cancel; GError **error; gboolean failed; } Parse;
static void fail(Parse *p, const char *message) {
    if (!p->failed) g_set_error_literal(p->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    p->failed = TRUE;
}
static MapShape *shape_new(Parse *p, const char *name, gboolean polygon) {
    MapShape *s = g_new0(MapShape, 1); s->points = g_array_new(FALSE, FALSE, sizeof(MapPoint));
    s->name = name && *name ? g_utf8_make_valid(name, -1) : NULL; s->polygon = polygon;
    s->x0 = s->y0 = G_MAXDOUBLE; s->x1 = s->y1 = -G_MAXDOUBLE;
    g_ptr_array_add(p->data->shapes, s); return s;
}
static void point_add(Parse *p, MapShape *s, double lon, double lat, gboolean move) {
    if (p->failed) return;
    if (!isfinite(lon) || !isfinite(lat) || fabs(lon) > 180 || fabs(lat) > 90) {
        fail(p, "Coordinates must be finite longitude/latitude values in WGS84."); return;
    }
    if (p->data->points >= MAP_MAX_POINTS) { fail(p, "This map exceeds the two million point limit."); return; }
    if ((p->data->points & 4095) == 0 && g_cancellable_set_error_if_cancelled(p->cancel, p->error)) {
        p->failed = TRUE; return;
    }
    MapData *d = p->data;
    double x = (lon + 180.) / 360.;
    if (!d->points) d->anchor = x;
    double previous = !move && s->points->len ? g_array_index(s->points, MapPoint, s->points->len - 1).x : d->anchor;
    x += round(previous - x); /* Keep a dateline-crossing track contiguous. */
    MapPoint point = {x, map_latitude_y(lat), move || !s->points->len};
    g_array_append_val(s->points, point); d->points++;
    s->x0 = MIN(s->x0, point.x); s->x1 = MAX(s->x1, point.x);
    s->y0 = MIN(s->y0, point.y); s->y1 = MAX(s->y1, point.y);
    d->x0 = MIN(d->x0, point.x); d->x1 = MAX(d->x1, point.x);
    d->y0 = MIN(d->y0, point.y); d->y1 = MAX(d->y1, point.y);
}
static gboolean number(JsonNode *n, double *out) {
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return FALSE;
    GType t = json_node_get_value_type(n);
    if (t != G_TYPE_DOUBLE && t != G_TYPE_INT64) return FALSE;
    *out = json_node_get_double(n); return TRUE;
}
static JsonArray *array(JsonNode *n) { return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL; }
static const char *string_member(JsonObject *o, const char *key) {
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}
static void json_point(Parse *p, MapShape *s, JsonNode *n, gboolean move) {
    JsonArray *a = array(n); double x, y;
    if (!a || json_array_get_length(a) < 2 || !number(json_array_get_element(a, 0), &x) ||
        !number(json_array_get_element(a, 1), &y)) { fail(p, "A GeoJSON position needs longitude and latitude."); return; }
    point_add(p, s, x, y, move);
}
static void json_line(Parse *p, MapShape *s, JsonNode *n) {
    JsonArray *a = array(n); if (!a) { fail(p, "Invalid GeoJSON coordinate array."); return; }
    for (guint i = 0; i < json_array_get_length(a) && !p->failed; i++) json_point(p, s, json_array_get_element(a, i), i == 0);
}
static void json_polygon(Parse *p, JsonNode *n, const char *name) {
    JsonArray *rings = array(n); if (!rings) { fail(p, "Invalid GeoJSON polygon."); return; }
    MapShape *s = shape_new(p, name, TRUE);
    for (guint i = 0; i < json_array_get_length(rings) && !p->failed; i++) json_line(p, s, json_array_get_element(rings, i));
}
static void json_geometry(Parse *p, JsonNode *n, const char *name, guint depth) {
    if (p->failed || !n || JSON_NODE_HOLDS_NULL(n)) return;
    if (depth > 64 || !JSON_NODE_HOLDS_OBJECT(n)) { fail(p, "Invalid or deeply nested GeoJSON geometry."); return; }
    JsonObject *o = json_node_get_object(n); const char *type = string_member(o, "type");
    if (!type) { fail(p, "GeoJSON is missing its type."); return; }
    if (!strcmp(type, "Feature")) {
        JsonNode *props = json_object_get_member(o, "properties");
        const char *label = props && JSON_NODE_HOLDS_OBJECT(props) ? string_member(json_node_get_object(props), "name") : NULL;
        json_geometry(p, json_object_get_member(o, "geometry"), label, depth + 1); return;
    }
    if (!strcmp(type, "FeatureCollection") || !strcmp(type, "GeometryCollection")) {
        JsonArray *items = array(json_object_get_member(o, !strcmp(type, "FeatureCollection") ? "features" : "geometries"));
        if (!items) { fail(p, "Invalid GeoJSON collection."); return; }
        for (guint i = 0; i < json_array_get_length(items) && !p->failed; i++) json_geometry(p, json_array_get_element(items, i), name, depth + 1);
        return;
    }
    JsonNode *coords = json_object_get_member(o, "coordinates");
    if (!strcmp(type, "Point")) json_point(p, shape_new(p, name, FALSE), coords, TRUE);
    else if (!strcmp(type, "LineString")) json_line(p, shape_new(p, name, FALSE), coords);
    else if (!strcmp(type, "Polygon")) json_polygon(p, coords, name);
    else if (!strcmp(type, "MultiPoint") || !strcmp(type, "MultiLineString") || !strcmp(type, "MultiPolygon")) {
        JsonArray *a = array(coords); if (!a) { fail(p, "Invalid GeoJSON multipart coordinates."); return; }
        for (guint i = 0; i < json_array_get_length(a) && !p->failed; i++) {
            JsonNode *item = json_array_get_element(a, i);
            if (!strcmp(type, "MultiPolygon")) json_polygon(p, item, name);
            else if (!strcmp(type, "MultiPoint")) json_point(p, shape_new(p, name, FALSE), item, TRUE);
            else json_line(p, shape_new(p, name, FALSE), item);
        }
    } else fail(p, "Unsupported GeoJSON geometry type.");
}
static gboolean tag(xmlNode *n, const char *name) { return n->type == XML_ELEMENT_NODE && !xmlStrcmp(n->name, BAD_CAST name); }
static char *xml_name(xmlNode *n) {
    for (xmlNode *c = n->children; c; c = c->next) if (tag(c, "name")) {
        xmlChar *s = xmlNodeGetContent(c); char *copy = g_strdup((const char *)s); xmlFree(s); return copy;
    }
    return NULL;
}
static gboolean parse_double(const char *s, double *value) {
    char *end; if (!s || !*s) return FALSE;
    *value = g_ascii_strtod(s, &end);
    while (g_ascii_isspace(*end)) end++;
    return end != s && !*end && isfinite(*value);
}
static void gpx_point(Parse *p, MapShape *s, xmlNode *n) {
    xmlChar *lat = xmlGetProp(n, BAD_CAST "lat"), *lon = xmlGetProp(n, BAD_CAST "lon"); double x, y;
    if (parse_double((char *)lon, &x) && parse_double((char *)lat, &y)) point_add(p, s, x, y, FALSE);
    else fail(p, "A GPX point is missing valid longitude/latitude.");
    xmlFree(lat); xmlFree(lon);
}
static void gpx_walk(Parse *p, xmlNode *n, const char *name, guint depth) {
    if (depth > 64) { fail(p, "This GPX document is nested too deeply."); return; }
    for (; n && !p->failed; n = n->next) {
        if (tag(n, "trk") || tag(n, "gpx")) {
            char *label = xml_name(n); gpx_walk(p, n->children, label ? label : name, depth + 1); g_free(label);
        } else if (tag(n, "trkseg") || tag(n, "rte")) {
            char *label = xml_name(n); MapShape *s = shape_new(p, label ? label : name, FALSE); g_free(label);
            for (xmlNode *c = n->children; c && !p->failed; c = c->next) if (tag(c, "trkpt") || tag(c, "rtept")) gpx_point(p, s, c);
        } else if (tag(n, "wpt")) {
            char *label = xml_name(n); MapShape *s = shape_new(p, label, FALSE); g_free(label); gpx_point(p, s, n);
        }
    }
}
static void kml_coords(Parse *p, MapShape *s, xmlNode *n) {
    xmlChar *raw = xmlNodeGetContent(n); const char *at = (char *)raw;
    gboolean first = TRUE;
    while (at && *at && !p->failed) {
        while (g_ascii_isspace(*at)) at++;
        if (!*at) break;
        char *end; double x = g_ascii_strtod(at, &end);
        if (at == end || *end != ',') { fail(p, "Invalid KML coordinates."); break; }
        at = end + 1; double y = g_ascii_strtod(at, &end);
        if (at == end || (*end && *end != ',' && !g_ascii_isspace(*end))) { fail(p, "Invalid KML latitude."); break; }
        at = end;
        if (*at == ',') { at++; (void)g_ascii_strtod(at, &end); if (at == end) { fail(p, "Invalid KML altitude."); break; } at = end; }
        if (*at && !g_ascii_isspace(*at)) { fail(p, "Invalid KML coordinate separator."); break; }
        point_add(p, s, x, y, first); first = FALSE;
    }
    xmlFree(raw);
}
static void kml_walk(Parse *p, xmlNode *n, const char *name, MapShape *shape, guint depth) {
    if (depth > 64) { fail(p, "This KML document is nested too deeply."); return; }
    for (; n && !p->failed; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        if (tag(n, "Placemark")) {
            char *label = xml_name(n); kml_walk(p, n->children, label, NULL, depth + 1); g_free(label);
        } else if (tag(n, "Polygon") || tag(n, "LineString") || tag(n, "Point")) {
            MapShape *s = shape_new(p, name, tag(n, "Polygon")); kml_walk(p, n->children, name, s, depth + 1);
        } else if (tag(n, "coordinates") && shape) kml_coords(p, shape, n);
        else kml_walk(p, n->children, name, shape, depth + 1);
    }
}
MapData *map_data_parse(const char *contents, gsize size, GCancellable *cancel, GError **error) {
    if (size > MAP_MAX_BYTES) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Map files are limited to 64 MiB."); return NULL; }
    MapData *d = g_new0(MapData, 1); d->shapes = g_ptr_array_new_with_free_func(shape_free);
    d->x0 = d->y0 = G_MAXDOUBLE; d->x1 = d->y1 = -G_MAXDOUBLE;
    Parse p = {d, cancel, error, FALSE};
    const char *at = contents; gsize remain = size;
    if (remain >= 3 && !memcmp(at, "\xef\xbb\xbf", 3)) { at += 3; remain -= 3; }
    while (remain && g_ascii_isspace(*at)) { at++; remain--; }
    if (remain && *at == '<') {
        xmlDoc *doc = xmlReadMemory(at, (int)remain, "map.xml", NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
        if (!doc) fail(&p, "This XML map could not be read.");
        else if (doc->intSubset || doc->extSubset) fail(&p, "XML document types and entities are not supported.");
        else {
            xmlNode *root = xmlDocGetRootElement(doc);
            if (root && tag(root, "gpx")) gpx_walk(&p, root, NULL, 0);
            else if (root && tag(root, "kml")) kml_walk(&p, root, NULL, NULL, 0);
            else fail(&p, "Open a GPX, KML, or GeoJSON map.");
        }
        if (doc) xmlFreeDoc(doc);
    } else {
        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, at, (gssize)remain, error)) p.failed = TRUE;
        else json_geometry(&p, json_parser_get_root(parser), NULL, 0);
        g_object_unref(parser);
    }
    if (!p.failed && !d->points) fail(&p, "This file contains no supported map coordinates.");
    if (p.failed) { map_data_free(d); return NULL; }
    for (guint i = d->shapes->len; i; i--) if (!((MapShape *)g_ptr_array_index(d->shapes, i - 1))->points->len) g_ptr_array_remove_index(d->shapes, i - 1);
    return d;
}
MapData *map_data_load(GFile *file, GCancellable *cancel, GError **error) {
    GFileInputStream *input = g_file_read(file, cancel, error); if (!input) return NULL;
    GByteArray *bytes = g_byte_array_new(); guint8 buffer[32768]; gssize n;
    while ((n = g_input_stream_read(G_INPUT_STREAM(input), buffer, sizeof buffer, cancel, error)) > 0) {
        if (bytes->len + n > MAP_MAX_BYTES) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Map files are limited to 64 MiB."); n = -1; break; }
        g_byte_array_append(bytes, buffer, (guint)n);
    }
    g_object_unref(input);
    MapData *d = n < 0 ? NULL : map_data_parse((const char *)bytes->data, bytes->len, cancel, error);
    g_byte_array_unref(bytes); return d;
}
