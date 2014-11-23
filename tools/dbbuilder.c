/* Copyright (c) 2014 - The libcangjie authors.
 *
 * This file is part of libcangjie.
 *
 * libcangjie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libcangjie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libcangjie.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <gom/gom.h>

#include <cangjie.h>


static GomAdapter *adapter;
static GomRepository *repository;
static GomResourceGroup *group;
static int num_pending_ops = 0;

#define BATCH_SIZE 100

typedef struct {
    const char *dbpath;
    const char **inputs;
    GMainLoop *loop;
} BuilderData;

void
usage (gchar *progname)
{
    g_print ("Usage: %s RESULTDB SOURCEFILE [SOURCEFILE ...]\n", progname);
}

static void
write_cb (GObject      *source_object,
	  GAsyncResult *res,
	  gpointer      user_data)
{
    GError *error = NULL;

    if (!gom_resource_group_write_finish (GOM_RESOURCE_GROUP (source_object), res, &error)) {
        g_warning ("%s: Error writing to the database: %s", g_get_prgname (),
                   error->message);
        g_error_free (error);
    }
    if (g_atomic_int_dec_and_test (&num_pending_ops)) {
        g_print ("All done, exiting\n");
        g_main_loop_quit (user_data);
    }
}

void
create_db (const gchar *dburi)
{
    GList *obj_types;
    GError *error = NULL;

    /* Connect to the DB */
    adapter = gom_adapter_new ();
    gom_adapter_open_sync (adapter, dburi, &error);
    g_assert_no_error (error);

    /* Create the table */
    repository = gom_repository_new (adapter);
    obj_types = g_list_prepend (NULL, GINT_TO_POINTER (CANGJIE_TYPE_CHAR));
    gom_repository_automatic_migrate_sync (repository, 1, obj_types, &error);
    g_assert_no_error (error);
}


gchar *
get_version_nick (GFile *tablefile) {
    gchar *basename = g_file_get_basename (tablefile);

    /* All table files are called table-XXXX.txt */
    gchar *hyphen = strstr (basename, "-");
    gchar *dot = g_strrstr (basename, ".");

    return g_strndup (hyphen + 1, dot - hyphen - 1);
}


void
parse_and_insert_line (const gchar *line, CangjieVersion version)
{
    GEnumClass *orientation_class = g_type_class_ref (CANGJIE_TYPE_ORIENTATION);

    gchar **tokens = g_strsplit (line, "\t", 15);
    gchar *cjchar = tokens[0];
    gchar *simpchar = tokens[1];
    gboolean zh = atoi (tokens[2]);
    gboolean big5 = atoi (tokens[3]);
    gboolean hkscs = atoi (tokens[4]);
    gboolean zhuyin = atoi (tokens[5]);
    gboolean kanji = atoi (tokens[6]);
    gboolean hiragana = atoi (tokens[7]);
    gboolean katakana = atoi (tokens[8]);
    gboolean punctuation = atoi (tokens[9]);
    gboolean symbol = atoi (tokens[10]);
    CangjieOrientation orientation = g_enum_get_value_by_nick (orientation_class, tokens[11])->value;
    gchar *code = tokens[12];
    gchar *shortcode = tokens[13];
    guint frequency = atoi (tokens[14]);

    CangjieChar *c;

    if ((g_strcmp0 (code, "") == 0) && (g_strcmp0 (shortcode, "") == 0)) {
        /* This character would be useless in the database */
        return;
    }

    c = cangjie_char_new (repository, cjchar, simpchar, zh, big5, hkscs,
                          zhuyin, kanji, hiragana, katakana, punctuation,
                          symbol, orientation, version, code, shortcode,
                          frequency);

    gom_resource_group_append (group, GOM_RESOURCE (c));

    g_object_unref (c);
    g_strfreev (tokens);
    g_type_class_unref (orientation_class);
}

static gboolean
generate_db (gpointer user_data)
{
    BuilderData *data = user_data;
    gint ret = 0;

    gint i;
    const gchar *tablepath;
    GFile *tablefile;
    GFileInputStream *stream;
    GDataInputStream *table;
    gchar *nick, *line;
    guint32 linenum = 0;
    gsize length;

    GEnumClass *version_class = g_type_class_ref (CANGJIE_TYPE_VERSION);
    CangjieVersion version;

    GError *error = NULL;
    GTimer *timer = g_timer_new ();
    guint num_items = 0;

    create_db (data->dbpath);

    group = gom_resource_group_new (repository);

    for (i = 0; data->inputs[i] != NULL; i++) {
        tablepath = data->inputs[i];
        tablefile = g_file_new_for_path (tablepath);

        nick = get_version_nick (tablefile);
        version = g_enum_get_value_by_nick (version_class, nick)->value;
        g_free (nick);

        stream = g_file_read (tablefile, NULL, &error);

        if (error != NULL) {
            ret = error->code;
            g_warning ("%s: Cannot read %s: %s\n", g_get_prgname(), tablepath,
                       error->message);
            g_error_free (error);
            g_object_unref (tablefile);
            return ret;
        }

        table = g_data_input_stream_new (G_INPUT_STREAM (stream));

        g_timer_reset (timer);

        while (TRUE) {
            linenum += 1;
            line = g_data_input_stream_read_line_utf8 (table, &length, NULL, &error);

            if (error != NULL) {
                ret = error->code;
                g_warning ("%s: Error reading line %d: %s\n", g_get_prgname (), linenum,
                           error->message);
                g_error_free (error);
                g_free (line);
                g_object_unref (table);
                g_object_unref (stream);
                g_object_unref (tablefile);
                return ret;
            }

            if (line == NULL) {
                /* We finished reading the file */
                break;
            }

            if (length == 0 || line[0] == '#') {
                /* Ignore empty and comment lines */
                g_free (line);
                continue;
            }

            parse_and_insert_line (line, version);
            num_items++;
            if (num_items == BATCH_SIZE) {
                g_atomic_int_inc (&num_pending_ops);
                gom_resource_group_write_async (group, write_cb, data->loop);
                g_object_unref (group);
                group = gom_resource_group_new (repository);


                num_items = 0;
            }

            g_free (line);
        }

        g_print ("Time taken to parse %s: %f seconds\n", tablepath, g_timer_elapsed (timer, NULL));

        g_object_unref (table);
        g_object_unref (stream);
        g_object_unref (tablefile);
    }

    /* Write the last batch */
    g_atomic_int_inc (&num_pending_ops);
    gom_resource_group_write_async (group, write_cb, data->loop);
    g_object_unref (group);

    return G_SOURCE_REMOVE;
}

int main (int argc, char **argv)
{
    BuilderData data;
    GError *error = NULL;
    GTimer *timer = g_timer_new ();

    /* Hard-code a UTF-8 locale.
     *
     * Our source data actually **is** in UTF-8, and the printing here is only
     * for debugging purpose. Also, there is no need for i18n of this tool.
     */
    setlocale (LC_ALL, "en_US.utf8");

    if (argc < 3) {
        usage (argv[0]);
        return -1;
    }

    data.dbpath = argv[1];
    if (g_file_test (data.dbpath, G_FILE_TEST_EXISTS)) {
        g_warning ("DB file already exists: %s\n", data.dbpath);
        return 1;
    }
    data.inputs = (const char **) &argv[2];
    data.loop = g_main_loop_new (NULL, TRUE);

    g_idle_add (generate_db, &data);

    g_main_loop_run (data.loop);

    gom_adapter_close_sync (adapter, &error);

    if (error != NULL) {
        g_warning ("%s: Error closing the connection to the database: %s",
                   g_get_prgname (), error->message);
        g_error_free (error);
        return 1;
    }

    g_object_unref (repository);
    g_object_unref (adapter);

    g_print ("Total time taken: %f seconds\n", g_timer_elapsed (timer, NULL));

    return 0;
}
