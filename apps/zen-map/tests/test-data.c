/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "map-data.h"
#include <math.h>
#include <string.h>

static MapData *read_text(const char *text) {
    GError *error = NULL; MapData *d = map_data_parse(text, strlen(text), NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(d); return d;
}
static void gpx(void) {
    MapData *d = read_text("<gpx xmlns='http://www.topografix.com/GPX/1/1'><trk><name>Ridge</name><trkseg><trkpt lat='38' lon='-122'/><trkpt lat='38.1' lon='-122.2'/></trkseg><trkseg><trkpt lat='38.3' lon='-122.3'/></trkseg></trk><rte><rtept lat='1' lon='2'/></rte><wpt lat='38.4' lon='-122.4'><name>Lookout</name></wpt></gpx>");
    g_assert_cmpuint(d->points, ==, 5); g_assert_cmpuint(d->shapes->len, ==, 4);
    g_assert_cmpstr(((MapShape *)g_ptr_array_index(d->shapes, 0))->name, ==, "Ridge"); map_data_free(d);
}
static void kml(void) {
    MapData *d = read_text("<kml xmlns='http://www.opengis.net/kml/2.2'><Document><Placemark><name>Park</name><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0,0 2,0,0 2,2,0 0,0,0</coordinates></LinearRing></outerBoundaryIs><innerBoundaryIs><LinearRing><coordinates>0.5,0.5 1,0.5 1,1 0.5,0.5</coordinates></LinearRing></innerBoundaryIs></Polygon></Placemark></Document></kml>");
    g_assert_cmpuint(d->points, ==, 8); g_assert_cmpuint(d->shapes->len, ==, 1);
    MapShape *s = g_ptr_array_index(d->shapes, 0); g_assert_true(s->polygon); g_assert_true(g_array_index(s->points, MapPoint, 4).move); map_data_free(d);
}
static void geojson(void) {
    MapData *d = read_text("{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\",\"properties\":{\"name\":\"Island\"},\"geometry\":{\"type\":\"GeometryCollection\",\"geometries\":[{\"type\":\"Point\",\"coordinates\":[-122,38]},{\"type\":\"MultiLineString\",\"coordinates\":[[[179.9,1],[-179.9,1]],[[179.8,2],[179.9,2]]]}]}}]}");
    g_assert_cmpuint(d->points, ==, 5); g_assert_cmpuint(d->shapes->len, ==, 3);
    MapShape *s = g_ptr_array_index(d->shapes, 1); g_assert_cmpfloat(s->x1 - s->x0, <, .001); map_data_free(d);
    d = read_text("{\"type\":\"MultiPolygon\",\"coordinates\":[[[[0,0],[1,0],[1,1],[0,0]],[[0.1,0.1],[0.2,0.1],[0.2,0.2],[0.1,0.1]]]]}");
    s = g_ptr_array_index(d->shapes, 0); g_assert_true(s->polygon); g_assert_cmpuint(s->points->len, ==, 8); map_data_free(d);
}
static void invalid(void) {
    const char *bad[] = {"", "{}", "{\"type\":\"Point\",\"coordinates\":[\"x\",10]}", "{\"type\":\"Point\",\"coordinates\":[0,91]}", "{\"type\":\"FeatureCollection\",\"features\":{}}", "<gpx><wpt lat='nan' lon='2'/></gpx>", "<!DOCTYPE gpx [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><gpx><name>&x;</name></gpx>", "<kml><Point><coordinates>1,x</coordinates></Point></kml>", NULL};
    for (guint i = 0; bad[i]; i++) { GError *e = NULL; MapData *d = map_data_parse(bad[i], strlen(bad[i]), NULL, &e); g_assert_null(d); g_assert_nonnull(e); g_clear_error(&e); }
    GCancellable *c = g_cancellable_new(); g_cancellable_cancel(c); GError *e = NULL;
    const char *point = "{\"type\":\"Point\",\"coordinates\":[0,0]}";
    MapData *d = map_data_parse(point, strlen(point), c, &e); g_assert_null(d); g_assert_error(e, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&e); g_object_unref(c);
}
static void projection(void) { for (double lat = -85; lat <= 85; lat += 1) g_assert_cmpfloat(fabs(map_y_latitude(map_latitude_y(lat)) - lat), <, 1e-10); }
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); g_test_add_func("/map/gpx", gpx); g_test_add_func("/map/kml", kml); g_test_add_func("/map/geojson", geojson); g_test_add_func("/map/invalid", invalid); g_test_add_func("/map/projection", projection); return g_test_run();
}
