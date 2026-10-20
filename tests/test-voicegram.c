/* test-voicegram.c - unit tests for .voice parsing/writing and ids */
#include "config.h"

#include <glib.h>
#include "gvs-voicegram.h"
#include "gvs-store.h"

static void
test_parse_client_file (void)
{
  /* Exactly what gnome-voice's gv_wizard_close() writes. */
  const gchar *xml =
    "<voice version='8.0.0'>\n"
    "  <station name='Ole Kristian Aamot' uri='http://myhost/GNOME.ogg'>\n"
    "    <stream>http://myhost/GNOME.ogg</stream>\n"
    "  </station>\n"
    "</voice>\n";
  g_autoptr(GError) error = NULL;
  g_autoptr(GvsVoicegram) vg = gvs_voicegram_parse (xml, -1, &error);

  g_assert_no_error (error);
  g_assert_nonnull (vg);
  g_assert_cmpstr (vg->version, ==, "8.0.0");
  g_assert_cmpstr (vg->name, ==, "Ole Kristian Aamot");
  g_assert_cmpstr (vg->uri, ==, "http://myhost/GNOME.ogg");
  g_assert_cmpstr (vg->stream, ==, "http://myhost/GNOME.ogg");
  g_assert_false (vg->has_coords);
}

static void
test_parse_location (void)
{
  const gchar *xml =
    "<?xml version=\"1.0\"?>\n"
    "<voice version=\"9.0.0\">\n"
    "  <station name=\"Sondre Lerche\" uri=\"http://www.sondrelerche.com/\">\n"
    "    <location lat=\"34.05\" lon=\"-118.24\">Los Angeles, California</location>\n"
    "    <stream>http://api.perceptron.stream:8000/192.ogg</stream>\n"
    "  </station>\n"
    "</voice>\n";
  g_autoptr(GError) error = NULL;
  g_autoptr(GvsVoicegram) vg = gvs_voicegram_parse (xml, -1, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (vg->location, ==, "Los Angeles, California");
  g_assert_true (vg->has_coords);
  g_assert_cmpfloat_with_epsilon (vg->lat, 34.05, 1e-9);
  g_assert_cmpfloat_with_epsilon (vg->lon, -118.24, 1e-9);
}

static void
test_bad_coords_dropped (void)
{
  const gchar *xml =
    "<voice><station name='x'><location lat='999' lon='abc'>Nowhere</location>"
    "</station></voice>";
  g_autoptr(GvsVoicegram) vg = gvs_voicegram_parse (xml, -1, NULL);

  g_assert_nonnull (vg);
  g_assert_false (vg->has_coords);
  g_assert_cmpstr (vg->location, ==, "Nowhere");
}

static void
test_reject (void)
{
  g_autoptr(GError) error = NULL;
  GvsVoicegram *vg;

  vg = gvs_voicegram_parse ("<radio/>", -1, &error);
  g_assert_null (vg);
  g_assert_error (error, GVS_VOICEGRAM_ERROR, GVS_VOICEGRAM_ERROR_INVALID);
  g_clear_error (&error);

  vg = gvs_voicegram_parse ("<voice version='1'></voice>", -1, &error);
  g_assert_null (vg);
  g_assert_error (error, GVS_VOICEGRAM_ERROR, GVS_VOICEGRAM_ERROR_NO_STATION);
  g_clear_error (&error);

  vg = gvs_voicegram_parse ("<voice><station>", -1, &error);
  g_assert_null (vg);
  g_assert_nonnull (error);
  g_clear_error (&error);

  vg = gvs_voicegram_parse ("<voice>\xff</voice>", -1, &error);
  g_assert_null (vg);
  g_assert_nonnull (error);
}

static void
test_roundtrip_escaping (void)
{
  g_autoptr(GvsVoicegram) vg = gvs_voicegram_new ();
  g_autoptr(GvsVoicegram) back = NULL;
  g_autofree gchar *xml = NULL;
  g_autoptr(GError) error = NULL;

  vg->id = g_strdup ("20261015T070800Z-0000abcd");
  vg->name = g_strdup ("Bjørn \"<Ålesund>\" & co");
  vg->location = g_strdup ("Ålesund");
  vg->stream = g_strdup ("http://x/voicegrams/a.ogg?x=1&y=2");
  g_assert_true (gvs_voicegram_set_coords (vg, "62.4722", "6.1495"));

  xml = gvs_voicegram_to_xml (vg);
  back = gvs_voicegram_parse (xml, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (back->id, ==, vg->id);
  g_assert_cmpstr (back->name, ==, vg->name);
  g_assert_cmpstr (back->location, ==, "Ålesund");
  g_assert_cmpstr (back->stream, ==, vg->stream);
  g_assert_cmpfloat_with_epsilon (back->lat, 62.4722, 1e-6);
}

static void
test_ids (void)
{
  g_autofree gchar *id = gvs_store_new_id ();

  g_assert_true (gvs_store_id_is_valid (id));
  g_assert_false (gvs_store_id_is_valid (""));
  g_assert_false (gvs_store_id_is_valid ("../etc/passwd"));
  g_assert_false (gvs_store_id_is_valid ("a/b"));
  g_assert_false (gvs_store_id_is_valid ("a.ogg"));
  g_assert_false (gvs_store_id_is_valid ("-rf"));
  g_assert_false (gvs_store_id_is_valid (NULL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/voicegram/parse-client-file", test_parse_client_file);
  g_test_add_func ("/voicegram/parse-location", test_parse_location);
  g_test_add_func ("/voicegram/bad-coords", test_bad_coords_dropped);
  g_test_add_func ("/voicegram/reject", test_reject);
  g_test_add_func ("/voicegram/roundtrip", test_roundtrip_escaping);
  g_test_add_func ("/store/ids", test_ids);
  return g_test_run ();
}
