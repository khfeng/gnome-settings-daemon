/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <libnotify/notify.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <cups/cups.h>
#include <cups/ppd.h>

static GDBusNodeInfo *npn_introspection_data = NULL;
static GDBusNodeInfo *pdi_introspection_data = NULL;

#define SCP_DBUS_NPN_NAME      "com.redhat.NewPrinterNotification"
#define SCP_DBUS_NPN_PATH      "/com/redhat/NewPrinterNotification"
#define SCP_DBUS_NPN_INTERFACE "com.redhat.NewPrinterNotification"

#define SCP_DBUS_PDI_NAME      "com.redhat.PrinterDriversInstaller"
#define SCP_DBUS_PDI_PATH      "/com/redhat/PrinterDriversInstaller"
#define SCP_DBUS_PDI_INTERFACE "com.redhat.PrinterDriversInstaller"

#define PACKAGE_KIT_BUS "org.freedesktop.PackageKit"
#define PACKAGE_KIT_PATH "/org/freedesktop/PackageKit"
#define PACKAGE_KIT_MODIFY_IFACE  "org.freedesktop.PackageKit.Modify"
#define PACKAGE_KIT_QUERY_IFACE  "org.freedesktop.PackageKit.Query"

#define SCP_BUS   "org.fedoraproject.Config.Printing"
#define SCP_PATH  "/org/fedoraproject/Config/Printing"
#define SCP_IFACE "org.fedoraproject.Config.Printing"

#define MECHANISM_BUS "org.opensuse.CupsPkHelper.Mechanism"

#define ALLOWED_CHARACTERS "abcdefghijklmnopqrtsuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_/"

#define DBUS_TIMEOUT           60000
#define DBUS_INSTALL_TIMEOUT 3600000

static const gchar npn_introspection_xml[] =
  "<node name='/com/redhat/NewPrinterNotification'>"
  "  <interface name='com.redhat.NewPrinterNotification'>"
  "    <method name='GetReady'>"
  "    </method>"
  "    <method name='NewPrinter'>"
  "      <arg type='i' name='status' direction='in'/>"
  "      <arg type='s' name='name' direction='in'/>"
  "      <arg type='s' name='mfg' direction='in'/>"
  "      <arg type='s' name='mdl' direction='in'/>"
  "      <arg type='s' name='des' direction='in'/>"
  "      <arg type='s' name='cmd' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static const gchar pdi_introspection_xml[] =
  "<node name='/com/redhat/PrinterDriversInstaller'>"
  "  <interface name='com.redhat.PrinterDriversInstaller'>"
  "    <method name='InstallDrivers'>"
  "      <arg type='s' name='mfg' direction='in'/>"
  "      <arg type='s' name='mdl' direction='in'/>"
  "      <arg type='s' name='cmd' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static GHashTable *
get_missing_executables (const gchar *ppd_file_name)
{
        GHashTable *executables = NULL;
        GDBusProxy *proxy;
        GVariant   *input;
        GVariant   *output;
        GVariant   *array;
        GError     *error = NULL;
        gint        i;

        if (!ppd_file_name)
                return NULL;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               SCP_BUS,
                                               SCP_PATH,
                                               SCP_IFACE,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return NULL;
        }

        input = g_variant_new ("(s)",
                               ppd_file_name);

        output = g_dbus_proxy_call_sync (proxy,
                                         "MissingExecutables",
                                         input,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         DBUS_TIMEOUT,
                                         NULL,
                                         &error);

        g_variant_unref (input);

        if (output && g_variant_n_children (output) == 1) {
                array = g_variant_get_child_value (output, 0);
                if (array) {
                        executables = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                             g_free, NULL);
                        for (i = 0; i < g_variant_n_children (array); i++) {
                                g_hash_table_insert (executables,
                                                     g_strdup (g_variant_get_string (
                                                       g_variant_get_child_value (array, i),
                                                       NULL)),
                                                     NULL);
                        }
                }
        }

        if (output) {
                g_variant_unref (output);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
        }

        g_object_unref (proxy);

        return executables;
}

static GHashTable *
find_packages_for_executables (GHashTable *executables)
{
        GHashTableIter  exec_iter;
        GHashTable     *packages = NULL;
        GDBusProxy     *proxy;
        GVariant       *input;
        GVariant       *output;
        gpointer        key, value;
        GError         *error = NULL;

        if (!executables || g_hash_table_size (executables) <= 0)
                return NULL;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               PACKAGE_KIT_BUS,
                                               PACKAGE_KIT_PATH,
                                               PACKAGE_KIT_QUERY_IFACE,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return NULL;
        }

        packages = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);

        g_hash_table_iter_init (&exec_iter, executables);
        while (g_hash_table_iter_next (&exec_iter, &key, &value)) {
                input = g_variant_new ("(ss)", (gchar *) key, "");

                output = g_dbus_proxy_call_sync (proxy,
                                                 "SearchFile",
                                                 input,
                                                 G_DBUS_CALL_FLAGS_NONE,
                                                 DBUS_TIMEOUT,
                                                 NULL,
                                                 &error);
                g_variant_unref (input);

                if (output) {
                        gboolean  installed;
                        gchar    *package;

                        g_variant_get (output,
                                       "(bs)",
                                       &installed,
                                       &package);
                        if (!installed)
                                g_hash_table_insert (packages, g_strdup (package), NULL);

                        g_variant_unref (output);
                } else {
                        g_warning ("%s", error->message);
                        g_error_free (error);
                }
        }

        g_object_unref (proxy);

        return packages;
}

static void
install_packages (GHashTable *packages)
{
        GVariantBuilder  array_builder;
        GHashTableIter   pkg_iter;
        GDBusProxy      *proxy;
        GVariant        *input;
        GVariant        *output;
        gpointer         key, value;
        GError          *error = NULL;

        if (!packages || g_hash_table_size (packages) <= 0)
                return;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               PACKAGE_KIT_BUS,
                                               PACKAGE_KIT_PATH,
                                               PACKAGE_KIT_MODIFY_IFACE,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return;
        }

        g_variant_builder_init (&array_builder, G_VARIANT_TYPE ("as"));

        g_hash_table_iter_init (&pkg_iter, packages);
        while (g_hash_table_iter_next (&pkg_iter, &key, &value)) {
                g_variant_builder_add (&array_builder,
                                       "s",
                                       (gchar *) key);
        }

        input = g_variant_new ("(uass)",
                0,
                &array_builder,
                "hide-finished");

        output = g_dbus_proxy_call_sync (proxy,
                                         "InstallPackageNames",
                                         input,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         DBUS_INSTALL_TIMEOUT,
                                         NULL,
                                         &error);

        g_variant_unref (input);

        if (output) {
                g_variant_unref (output);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
        }

        g_object_unref (proxy);
}

static gchar *
get_best_ppd (gchar *device_id,
              gchar *device_make_and_model,
              gchar *device_uri)
{
        GDBusProxy  *proxy;
        GVariant    *input;
        GVariant    *output;
        GVariant    *array;
        GVariant    *tuple;
        GError      *error = NULL;
        gchar       *ppd_name = NULL;
        gint         i, j;
        static const char * const match_levels[] = {
                   "exact-cmd",
                   "exact",
                   "close",
                   "generic",
                   "none"};

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               SCP_BUS,
                                               SCP_PATH,
                                               SCP_IFACE,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return NULL;
        }

        input = g_variant_new ("(sss)",
                               device_id ? device_id : "",
                               device_make_and_model ? device_make_and_model : "",
                               device_uri ? device_uri : "");

        output = g_dbus_proxy_call_sync (proxy,
                                         "GetBestDrivers",
                                         input,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         DBUS_TIMEOUT,
                                         NULL,
                                         &error);

        g_variant_unref (input);

        if (output && g_variant_n_children (output) >= 1) {
                array = g_variant_get_child_value (output, 0);
                if (array)
                        for (j = 0; j < G_N_ELEMENTS (match_levels) && ppd_name == NULL; j++)
                                for (i = 0; i < g_variant_n_children (array) && ppd_name == NULL; i++) {
                                        tuple = g_variant_get_child_value (array, i);
                                        if (tuple && g_variant_n_children (tuple) == 2) {
                                                if (g_strcmp0 (g_variant_get_string (
                                                                   g_variant_get_child_value (tuple, 1),
                                                                   NULL), match_levels[j]) == 0)
                                                        ppd_name = g_strdup (g_variant_get_string (
                                                                                 g_variant_get_child_value (tuple, 0),
                                                                                 NULL));
                                        }
                                }
        }

        if (output) {
                g_variant_unref (output);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
        }

        g_object_unref (proxy);

        return ppd_name;
}

static gchar *
get_tag_value (const gchar *tag_string,
               const gchar *tag_name)
{
        gchar **tag_string_splitted;
        gchar  *tag_value = NULL;
        gint    tag_name_length;
        gint    i;

        if (!tag_string ||
            !tag_name)
                return NULL;

        tag_name_length = strlen (tag_name);
        tag_string_splitted = g_strsplit (tag_string, ";", 0);
        if (tag_string_splitted) {
                for (i = 0; i < g_strv_length (tag_string_splitted); i++)
                        if (g_ascii_strncasecmp (tag_string_splitted[i], tag_name, tag_name_length) == 0)
                                if (strlen (tag_string_splitted[i]) > tag_name_length + 1)
                                        tag_value = g_strdup (tag_string_splitted[i] + tag_name_length + 1);

                g_strfreev (tag_string_splitted);
        }

        return tag_value;
}

static gchar *
create_name (gchar *device_id)
{
        cups_dest_t *dests;
        gboolean     already_present = FALSE;
        gchar       *name = NULL;
        gchar       *new_name = NULL;
        gint         num_dests;
        gint         name_index = 2;
        gint         j;

        g_return_val_if_fail (device_id != NULL, NULL);

        name = get_tag_value (device_id, "mdl");
        if (!name)
                name = get_tag_value (device_id, "model");

        if (name)
                name = g_strcanon (name, ALLOWED_CHARACTERS, '-');

        num_dests = cupsGetDests (&dests);
        do {
                if (already_present) {
                        new_name = g_strdup_printf ("%s-%d", name, name_index);
                        name_index++;
                } else {
                        new_name = g_strdup (name);
                }

                already_present = FALSE;
                for (j = 0; j < num_dests; j++)
                        if (g_strcmp0 (dests[j].name, new_name) == 0)
                                already_present = TRUE;

                if (already_present) {
                        g_free (new_name);
                } else {
                        g_free (name);
                        name = new_name;
                }
        } while (already_present);
        cupsFreeDests (num_dests, dests);

        return name;
}

static gboolean
add_printer (gchar *printer_name,
             gchar *device_uri,
             gchar *ppd_name,
             gchar *info,
             gchar *location)
{
        cups_dest_t *dests;
        GDBusProxy  *proxy;
        gboolean     success = FALSE;
        GVariant    *input;
        GVariant    *output;
        GError      *error = NULL;
        gint         num_dests;
        gint         i;

        if (!printer_name || !device_uri || !ppd_name)
                return FALSE;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               MECHANISM_BUS,
                                               "/",
                                               MECHANISM_BUS,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return FALSE;
        }

        input = g_variant_new ("(sssss)",
                               printer_name ? printer_name : "",
                               device_uri ? device_uri : "",
                               ppd_name ? ppd_name : "",
                               info ? info : "",
                               location ? location : "");

        output = g_dbus_proxy_call_sync (proxy,
                                         "PrinterAdd",
                                         input,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         DBUS_TIMEOUT,
                                         NULL,
                                         &error);

        if (output) {
                g_variant_unref (output);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
        }

        g_variant_unref (input);
        g_object_unref (proxy);

        num_dests = cupsGetDests (&dests);
        for (i = 0; i < num_dests; i++)
                if (g_strcmp0 (dests[i].name, printer_name) == 0)
                        success = TRUE;
        cupsFreeDests (num_dests, dests);

        return success;
}

static gboolean
printer_set_enabled (const gchar *printer_name,
                     gboolean     enabled)
{
        GDBusProxy *proxy;
        gboolean    result = TRUE;
        GVariant   *input;
        GVariant   *output;
        GError     *error = NULL;

        if (!printer_name)
                return FALSE;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               MECHANISM_BUS,
                                               "/",
                                               MECHANISM_BUS,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return FALSE;
        }

        input = g_variant_new ("(sb)",
                               printer_name,
                               enabled);

        output = g_dbus_proxy_call_sync (proxy,
                                         "PrinterSetEnabled",
                                         input,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         DBUS_TIMEOUT,
                                         NULL,
                                         &error);

        if (output) {
                g_variant_unref (output);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
                result = FALSE;
        }

        g_variant_unref (input);
        g_object_unref (proxy);

        return result;
}

static gboolean
printer_set_accepting_jobs (const gchar *printer_name,
                            gboolean     accepting_jobs,
                            const gchar *reason)
{
        GDBusProxy *proxy;
        gboolean    result = TRUE;
        GVariant   *input;
        GVariant   *output;
        GError     *error = NULL;

        if (!printer_name)
                return FALSE;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               MECHANISM_BUS,
                                               "/",
                                               MECHANISM_BUS,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return FALSE;
        }

        input = g_variant_new ("(sbs)",
                               printer_name,
                               accepting_jobs,
                               reason ? reason : "");

        output = g_dbus_proxy_call_sync (proxy,
                                         "PrinterSetAcceptJobs",
                                         input,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         DBUS_TIMEOUT,
                                         NULL,
                                         &error);

        if (output) {
                g_variant_unref (output);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
                result = FALSE;
        }

        g_variant_unref (input);
        g_object_unref (proxy);

        return result;
}

static ipp_t *
execute_maintenance_command (const char *printer_name,
                             const char *command,
                             const char *title)
{
        http_t *http;
        GError *error = NULL;
        ipp_t  *request = NULL;
        ipp_t  *response = NULL;
        gchar  *file_name = NULL;
        char   *uri;
        int     fd = -1;

        http = httpConnectEncrypt (cupsServer (),
                                   ippPort (),
                                   cupsEncryption ());

        if (!http)
                return NULL;

        request = ippNewRequest (IPP_PRINT_JOB);

        uri = g_strdup_printf ("ipp://localhost/printers/%s",
                               printer_name);

        ippAddString (request,
                      IPP_TAG_OPERATION,
                      IPP_TAG_URI,
                      "printer-uri",
                      NULL,
                      uri);

        g_free (uri);

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name",
                      NULL, title);

        ippAddString (request, IPP_TAG_JOB, IPP_TAG_MIMETYPE, "document-format",
                      NULL, "application/vnd.cups-command");

        fd = g_file_open_tmp ("ccXXXXXX", &file_name, &error);

        if (fd != -1) {
                FILE *file;

                file = fdopen (fd, "w");
                fprintf (file, "#CUPS-COMMAND\n");
                fprintf (file, "%s\n", command);
                fclose (file);

                response = cupsDoFileRequest (http, request, "/", file_name);
                g_unlink (file_name);
        } else {
                g_warning ("%s", error->message);
                g_error_free (error);
        }

        g_free (file_name);
        httpClose (http);

        return response;
}

static char *
get_dest_attr (const char *dest_name,
               const char *attr)
{
        cups_dest_t *dests;
        int          num_dests;
        cups_dest_t *dest;
        const char  *value;
        char        *ret;

        if (dest_name == NULL)
                return NULL;

        ret = NULL;

        num_dests = cupsGetDests (&dests);
        if (num_dests < 1) {
                g_debug ("Unable to get printer destinations");
                return NULL;
        }

        dest = cupsGetDest (dest_name, NULL, num_dests, dests);
        if (dest == NULL) {
                g_debug ("Unable to find a printer named '%s'", dest_name);
                goto out;
        }

        value = cupsGetOption (attr, dest->num_options, dest->options);
        if (value == NULL) {
                g_debug ("Unable to get %s for '%s'", attr, dest_name);
                goto out;
        }
        ret = g_strdup (value);
out:
        cupsFreeDests (num_dests, dests);

        return ret;
}

static void
printer_autoconfigure (gchar *printer_name)
{
        gchar *commands;
        gchar *commands_lowercase;
        ipp_t *response = NULL;

        if (!printer_name)
                return;

        commands = get_dest_attr (printer_name, "printer-commands");
        commands_lowercase = g_ascii_strdown (commands, -1);

        if (g_strrstr (commands_lowercase, "autoconfigure")) {
                response = execute_maintenance_command (printer_name,
                                                        "AutoConfigure",
                                                        ("Automatic configuration"));
                if (response) {
                        if (response->state == IPP_ERROR)
                                g_warning ("An error has occured during automatic configuration of new printer.");
                        ippDelete (response);
                }
        }
        g_free (commands);
        g_free (commands_lowercase);
}


static void
set_default_paper_size (const gchar *printer_name,
                        const gchar *ppd_file_name)
{
        ppd_file_t  *ppd_file = NULL;
        GDBusProxy  *proxy;
        GVariant    *input;
        GVariant    *output;
        GError      *error = NULL;
        gchar      **value = NULL;
        gchar       *paper_size;
        char        *locale;
        gint         i, j, k;

        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL,
                                               MECHANISM_BUS,
                                               "/",
                                               MECHANISM_BUS,
                                               NULL,
                                               &error);

        if (!proxy) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return;
        }

        /* Set default PaperSize according to the locale */
        locale = setlocale (LC_PAPER, NULL);
        if (!locale)
                locale = setlocale (LC_MESSAGES, NULL);

        if (!locale) {
                g_object_unref (proxy);
                return;
        }

        /* CLDR 2.0 alpha
         * http://unicode.org/repos/cldr-tmp/trunk/diff/supplemental/territory_language_information.html
         */
        if (g_regex_match_simple ("[^_.@]{2,3}_(BZ|CA|CL|CO|CR|GT|MX|NI|PA|PH|PR|SV|US|VE)",
                                  locale, G_REGEX_ANCHORED, G_REGEX_MATCH_ANCHORED))
                paper_size = g_strdup ("Letter");
        else
                paper_size = g_strdup ("A4");

        if (ppd_file_name) {
                ppd_file = ppdOpenFile (ppd_file_name);
                if (ppd_file) {
                        ppdMarkDefaults (ppd_file);
                        for (i = 0; i < ppd_file->num_groups; i++)
                                for (j = 0; j < ppd_file->groups[i].num_options; j++)
                                        if (g_strcmp0 ("PageSize", ppd_file->groups[i].options[j].keyword) == 0) {
                                                for (k = 0; k < ppd_file->groups[i].options[j].num_choices; k++) {
                                                        if (g_ascii_strncasecmp (paper_size,
                                                                                 ppd_file->groups[i].options[j].choices[k].choice,
                                                                                 strlen (paper_size)) == 0 &&
                                                            !ppd_file->groups[i].options[j].choices[k].marked) {
                                                            value = g_new0 (gchar *, 2);
                                                            value[0] = g_strdup (ppd_file->groups[i].options[j].choices[k].choice);
                                                            break;
                                                        }
                                                }
                                                break;
                                        }
                      ppdClose (ppd_file);
                }
        }

        if (value) {
                GVariant *array;

                array = g_variant_new_strv ((const gchar * const *)value, -1);

                input = g_variant_new ("(ssv)",
                                       printer_name ? printer_name : "",
                                       "PageSize-default",
                                       array);

                output = g_dbus_proxy_call_sync (proxy,
                                                 "PrinterAddOptionDefault",
                                                 input,
                                                 G_DBUS_CALL_FLAGS_NONE,
                                                 DBUS_TIMEOUT,
                                                 NULL,
                                                 &error);

                if (output) {
                        g_variant_unref (output);
                } else {
                        g_warning ("%s", error->message);
                        g_error_free (error);
                }

                g_variant_unref (input);
                g_variant_unref (array);
                g_strfreev (value);
        }

        g_free (paper_size);
        g_object_unref (proxy);
}

/*
 * Setup new printer and returns TRUE if successful.
 */
static gboolean
setup_printer (gchar *device_id,
               gchar *device_make_and_model,
               gchar *device_uri)
{
        gboolean  success = FALSE;
        gchar    *ppd_name;
        gchar    *printer_name;

        ppd_name = get_best_ppd (device_id, device_make_and_model, device_uri);
        printer_name = create_name (device_id);

        if (!ppd_name || !printer_name || !device_uri) {
                g_free (ppd_name);
                g_free (printer_name);
                return FALSE;
        }

        success = add_printer (printer_name, device_uri,
                               ppd_name, NULL, NULL);

        /* Set some options of the new printer */
        if (success) {
                const char *ppd_file_name;

                printer_set_accepting_jobs (printer_name, TRUE, NULL);
                printer_set_enabled (printer_name, TRUE);
                printer_autoconfigure (printer_name);

                ppd_file_name = cupsGetPPD (printer_name);

                if (ppd_file_name) {
                        GHashTable *executables;
                        GHashTable *packages;

                        set_default_paper_size (printer_name, ppd_file_name);

                        executables = get_missing_executables (ppd_file_name);
                        packages = find_packages_for_executables (executables);
                        install_packages (packages);

                        if (executables)
                                g_hash_table_destroy (executables);
                        if (packages)
                                g_hash_table_destroy (packages);
                        g_unlink (ppd_file_name);
                }
        }

        g_free (printer_name);
        g_free (ppd_name);

        return success;
}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
        gchar *primary_text = NULL;
        gchar *secondary_text = NULL;
        gchar *name = NULL;
        gchar *mfg = NULL;
        gchar *mdl = NULL;
        gchar *des = NULL;
        gchar *cmd = NULL;
        gchar *device = NULL;
        gchar *device_id;
        gchar *make_and_model;
        gint   status = 0;

        if (g_strcmp0 (method_name, "GetReady") == 0) {
                /* Translators: We are configuring new printer */
                primary_text = g_strdup (_("Configuring new printer"));
                /* Translators: Just wait */
                secondary_text = g_strdup (_("Please wait..."));

                g_dbus_method_invocation_return_value (invocation,
                                                       NULL);
        }
        else if (g_strcmp0 (method_name, "NewPrinter") == 0) {
                if (g_variant_n_children (parameters) == 6) {
                        g_variant_get (parameters, "(i&s&s&s&s&s)",
                               &status,
                               &name,
                               &mfg,
                               &mdl,
                               &des,
                               &cmd);
                }

                if (g_strrstr (name, "/")) {
                        /* name is a URI, no queue was generated, because no suitable
                         * driver was found
                         */

                        device_id = g_strdup_printf ("MFG:%s;MDL:%s;DES:%s;CMD:%s;", mfg, mdl, des, cmd);
                        make_and_model = g_strdup_printf ("%s %s", mfg, mdl);

                        if (!setup_printer (device_id, make_and_model, name)) {

                                /* Translators: We have no driver installed for this printer */
                                primary_text = g_strdup (_("Missing printer driver"));

                                if ((mfg && mdl) || des) {
                                        if (mfg && mdl)
                                                device = g_strdup_printf ("%s %s", mfg, mdl);
                                        else
                                                device = g_strdup (des);

                                        /* Translators: We have no driver installed for the device */
                                        secondary_text = g_strdup_printf (_("No printer driver for %s."), device);
                                        g_free (device);
                                }
                                else
                                        /* Translators: We have no driver installed for this printer */
                                        secondary_text = g_strdup (_("No driver for this printer."));
                        }

                        g_free (make_and_model);
                        g_free (device_id);
                }
                else {
                        /* name is the name of the queue which hal_lpadmin has set up
                         * automatically.
                         */

                        const char *ppd_file_name;

                        ppd_file_name = cupsGetPPD (name);
                        if (ppd_file_name) {
                                GHashTable *executables;
                                GHashTable *packages;

                                executables = get_missing_executables (ppd_file_name);
                                packages = find_packages_for_executables (executables);
                                install_packages (packages);

                                if (executables)
                                        g_hash_table_destroy (executables);
                                if (packages)
                                        g_hash_table_destroy (packages);
                                g_unlink (ppd_file_name);
                        }
                }

                g_dbus_method_invocation_return_value (invocation,
                                                       NULL);
        }
        else if (g_strcmp0 (method_name, "InstallDrivers") == 0) {
                GDBusProxy *proxy;
                GError     *error = NULL;

                if (g_variant_n_children (parameters) == 3) {
                        g_variant_get (parameters, "(&s&s&s)",
                               &mfg,
                               &mdl,
                               &cmd);
                }

                if (mfg && mdl)
                        device = g_strdup_printf ("MFG:%s;MDL:%s;", mfg, mdl);

                proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       NULL,
                                                       PACKAGE_KIT_BUS,
                                                       PACKAGE_KIT_PATH,
                                                       PACKAGE_KIT_MODIFY_IFACE,
                                                       NULL,
                                                       &error);

                if (!proxy) {
                        g_warning ("%s", error->message);
                        g_error_free (error);
                }

                if (proxy && device) {
                        GVariantBuilder *builder;
                        GVariant        *input;
                        GVariant        *output;

                        builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
                        g_variant_builder_add (builder, "s", device);

                        input = g_variant_new ("(uass)",
                                               0,
                                               builder,
                                               "hide-finished");

                        output = g_dbus_proxy_call_sync (proxy,
                                                         "InstallPrinterDrivers",
                                                         input,
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         DBUS_INSTALL_TIMEOUT,
                                                         NULL,
                                                         &error);

                        if (output) {
                                g_variant_unref (output);
                        } else {
                                g_warning ("%s", error->message);
                                g_error_free (error);
                        }

                        g_variant_unref (input);
                        g_variant_builder_unref (builder);
                        g_object_unref (proxy);
                }

                g_dbus_method_invocation_return_value (invocation,
                                                       NULL);
        }

        if (primary_text) {
                NotifyNotification *notification;
                notification = notify_notification_new (primary_text,
                                                        secondary_text,
                                                        "printer-symbolic");
                notify_notification_set_app_name (notification, _("Printers"));
                notify_notification_set_hint (notification, "transient", g_variant_new_boolean (TRUE));

                notify_notification_show (notification, NULL);
                g_object_unref (notification);
                g_free (primary_text);
                g_free (secondary_text);
        }
}

static const GDBusInterfaceVTable interface_vtable =
{
  handle_method_call,
  NULL,
  NULL
};

static void
on_npn_bus_acquired (GDBusConnection *connection,
                     const gchar     *name,
                     gpointer         user_data)
{
  guint registration_id;

  registration_id = g_dbus_connection_register_object (connection,
                                                       SCP_DBUS_NPN_PATH,
                                                       npn_introspection_data->interfaces[0],
                                                       &interface_vtable,
                                                       NULL,
                                                       NULL,
                                                       NULL);
  g_assert (registration_id > 0);
}

static void
on_pdi_bus_acquired (GDBusConnection *connection,
                     const gchar     *name,
                     gpointer         user_data)
{
  guint registration_id;

  registration_id = g_dbus_connection_register_object (connection,
                                                       SCP_DBUS_PDI_PATH,
                                                       pdi_introspection_data->interfaces[0],
                                                       &interface_vtable,
                                                       NULL,
                                                       NULL,
                                                       NULL);
  g_assert (registration_id > 0);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}

int
main (int argc, char *argv[])
{
  guint npn_owner_id;
  guint pdi_owner_id;
  GMainLoop *loop;

  g_type_init ();

  notify_init ("gnome-settings-daemon-printer");

  npn_introspection_data =
          g_dbus_node_info_new_for_xml (npn_introspection_xml, NULL);
  g_assert (npn_introspection_data != NULL);

  pdi_introspection_data =
          g_dbus_node_info_new_for_xml (pdi_introspection_xml, NULL);
  g_assert (pdi_introspection_data != NULL);

  npn_owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                 SCP_DBUS_NPN_NAME,
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_npn_bus_acquired,
                                 on_name_acquired,
                                 on_name_lost,
                                 NULL,
                                 NULL);

  pdi_owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                 SCP_DBUS_PDI_NAME,
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_pdi_bus_acquired,
                                 on_name_acquired,
                                 on_name_lost,
                                 NULL,
                                 NULL);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (npn_owner_id);
  g_bus_unown_name (pdi_owner_id);

  g_dbus_node_info_unref (npn_introspection_data);
  g_dbus_node_info_unref (pdi_introspection_data);

  return 0;
}
