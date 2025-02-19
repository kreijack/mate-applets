/* battstat        A MATE battery meter for laptops.
 * Copyright (C) 2000 by Jörgen Pehrson <jp@spektr.eu.org>
 * Copyright (C) 2002 Free Software Foundation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 $Id$
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_ERR_H
#include <err.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include <mate-panel-applet.h>
#include <mate-panel-applet-gsettings.h>

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

#include "battstat.h"
#include "battstat-preferences.h"

#define BATTSTAT_SCHEMA "org.mate.panel.applet.battstat"

static gboolean check_for_updates (gpointer data);

static void about_cb (GtkAction *, ProgressData *);

static void help_cb (GtkAction *, ProgressData *);

static const GtkActionEntry battstat_menu_actions [] = {
    { "BattstatProperties", "document-properties", N_("_Preferences"),
        NULL, NULL,
        G_CALLBACK (prop_cb) },
    { "BattstatHelp", "help-browser", N_("_Help"),
        NULL, NULL,
        G_CALLBACK (help_cb) },
    { "BattstatAbout", "help-about", N_("_About"),
        NULL, NULL,
        G_CALLBACK (about_cb) }
};

#define AC_POWER_STRING _("System is running on AC power")
#define DC_POWER_STRING _("System is running on battery power")

/* Our backends may be either event driven or poll-based.
 * If they are event driven then we know this the first time we
 * receive an event.
 */
static gboolean event_driven = FALSE;
static GSList *instances;

static void
status_change_callback (void)
{
    GSList *instance;

    for (instance = instances; instance; instance = instance->next)
    {
        ProgressData *battstat = instance->data;

        if (battstat->timeout_id)
        {
            g_source_remove (battstat->timeout_id);
            battstat->timeout_id = 0;
        }

        check_for_updates (battstat);
    }

    event_driven = TRUE;
}

/* The following two functions keep track of how many instances of the applet
   are currently running.  When the first instance is started, some global
   initialisation is done.  When the last instance exits, cleanup occurs.

   The teardown code here isn't entirely complete (for example, it doesn't
   deallocate the GdkColors or free the GdkPixmaps.  This is OK so long
   as the process quits immediately when the last applet is removed (which
   it does.)
*/
static const char *
static_global_initialisation (ProgressData *battstat)
{
    gboolean first_time;
    const char *err;

    first_time = !instances;

    instances = g_slist_prepend (instances, battstat);

    if (!first_time)
        return NULL;

    err = power_management_initialise (status_change_callback);

    return err;
}

static void
static_global_teardown (ProgressData *battstat)
{
    instances = g_slist_remove (instances, battstat);

    /* remaining instances... */
    if (instances)
        return;

    /* instances == 0 */

    power_management_cleanup ();
}

/* Pop up an error dialog on the same screen as 'applet' saying 'msg'.
 */
static void
battstat_error_dialog (GtkWidget  *applet,
                       const char *msg)
{
    GtkWidget *dialog;

    dialog = gtk_message_dialog_new (NULL, 0, GTK_MESSAGE_ERROR,
                                     GTK_BUTTONS_OK, "%s", msg);

    gtk_window_set_screen (GTK_WINDOW (dialog),
                           gtk_widget_get_screen (GTK_WIDGET (applet)));

    g_signal_connect_swapped (dialog, "response",
                              G_CALLBACK (gtk_widget_destroy),
                              G_OBJECT (dialog));

    gtk_widget_show_all (dialog);
}

/* Format a string describing how much time is left to fully (dis)charge
   the battery. The return value must be g_free ()d.
*/
static char *
get_remaining (BatteryStatus *info)
{
    int hours;
    int mins;

    hours = info->minutes / 60;
    mins = info->minutes % 60;

    if (info->on_ac_power && !info->charging)
        return g_strdup_printf (_("Battery charged (%d%%)"), info->percent);
    else if (info->minutes < 0 && !info->on_ac_power)
        return g_strdup_printf (_("Unknown time (%d%%) remaining"), info->percent);
    else if (info->minutes < 0 && info->on_ac_power)
        return g_strdup_printf (_("Unknown time (%d%%) until charged"), info->percent);
    else
        if (hours == 0)
            if (!info->on_ac_power)
                return g_strdup_printf (ngettext ("%d minute (%d%%) remaining",
                                                  "%d minutes (%d%%) remaining",
                                                  mins),
                                        mins, info->percent);
            else
                return g_strdup_printf (ngettext ("%d minute until charged (%d%%)",
                                                  "%d minutes until charged (%d%%)",
                                                  mins),
                                        mins, info->percent);
        else if (mins == 0)
            if (!info->on_ac_power)
                return g_strdup_printf (ngettext ("%d hour (%d%%) remaining",
                                                  "%d hours (%d%%) remaining",
                                                  hours),
                                        hours, info->percent);
            else
                return g_strdup_printf (ngettext ("%d hour until charged (%d%%)",
                                                  "%d hours until charged (%d%%)",
                                                  hours),
                                        hours, info->percent);
        else
            if (!info->on_ac_power)
                /* TRANSLATOR: "%d %s %d %s" are "%d hours %d minutes"
                 * Swap order with "%2$s %2$d %1$s %1$d if needed */
                return g_strdup_printf (_("%d %s %d %s (%d%%) remaining"),
                                        hours, ngettext ("hour", "hours", hours),
                                        mins, ngettext ("minute", "minutes", mins),
                                        info->percent);
            else
                /* TRANSLATOR: "%d %s %d %s" are "%d hours %d minutes"
                 * Swap order with "%2$s %2$d %1$s %1$d if needed */
                return g_strdup_printf (_("%d %s %d %s until charged (%d%%)"),
                                        hours, ngettext ("hour", "hours", hours),
                                        mins, ngettext ("minute", "minutes", mins),
                                        info->percent);
}

static gboolean
battery_full_notify (GtkWidget *applet)
{
#ifdef HAVE_LIBNOTIFY
    GError *error = NULL;
    GdkPixbuf *icon;
    gboolean result;

    if (!notify_is_initted () && !notify_init (_("Battery Monitor")))
        return FALSE;

    icon = gtk_icon_theme_load_icon_for_scale (gtk_icon_theme_get_default (),
                                               "battery",
                                               48,
                                               gtk_widget_get_scale_factor (applet),
                                               GTK_ICON_LOOKUP_USE_BUILTIN,
                                               NULL);

    NotifyNotification *n = notify_notification_new (_("Your battery is now fully recharged"),
                                                     "", /* "battery" */ NULL);

    notify_notification_set_image_from_pixbuf (n, icon);
    g_object_unref (icon);

    result = notify_notification_show (n, &error);

    if (error)
    {
        g_warning ("%s", error->message);
        g_error_free (error);
    }

    g_object_unref (G_OBJECT (n));

    return result;
#else
    return FALSE;
#endif
}

/* Show a dialog notifying the user that their battery is done charging. */
static void
battery_full_dialog (GtkWidget *applet)
{
  /* first attempt to use libnotify */
    if (battery_full_notify (applet))
        return;

    GtkWidget *dialog, *hbox, *image, *label;
    cairo_surface_t *surface;

    gchar *new_label;
    dialog = gtk_dialog_new_with_buttons (_("Battery Notice"),
                                          NULL,
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          "gtk-ok",
                                          GTK_RESPONSE_ACCEPT,
                                          NULL);
    g_signal_connect_swapped (dialog, "response",
                              G_CALLBACK (gtk_widget_destroy),
                              dialog);

    gtk_container_set_border_width (GTK_CONTAINER (dialog), 6);
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    surface = gtk_icon_theme_load_surface (gtk_icon_theme_get_default (),
                                           "battery",
                                           48,
                                           gtk_widget_get_scale_factor (applet),
                                           NULL,
                                           GTK_ICON_LOOKUP_USE_BUILTIN,
                                           NULL);

    image = gtk_image_new_from_surface (surface);
    cairo_surface_destroy (surface);
    gtk_box_pack_start (GTK_BOX (hbox), image, TRUE, TRUE, 6);
    new_label = g_strdup_printf ("<span weight=\"bold\" size=\"larger\">%s</span>",
                                 _("Your battery is now fully recharged"));

    label = gtk_label_new (new_label);
    g_free (new_label);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
    gtk_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 6);
    gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                       hbox);
    gtk_window_set_keep_above (GTK_WINDOW (dialog), TRUE);
    gtk_window_stick (GTK_WINDOW (dialog));
    gtk_window_set_skip_pager_hint (GTK_WINDOW (dialog), TRUE);
    gtk_window_set_focus_on_map (GTK_WINDOW (dialog), FALSE);
    gtk_widget_show_all (dialog);
}

/* Destroy the low battery notification dialog and mark it as such.
 */
static void
battery_low_dialog_destroy (ProgressData *battstat)
{
    gtk_widget_destroy (battstat->battery_low_dialog);
    battstat->battery_low_dialog = NULL;
    battstat->battery_low_label = NULL;
}

/* Determine if suspend is unsupported.  For the time being this involves
 * distribution-specific magic :(
 */
/* #define HAVE_PMI */
static gboolean
is_suspend_unavailable (void)
{
#ifdef HAVE_PMI
    int status;

    status = system ("pmi query suspend");

    /* -1 - fail (pmi unavailable?).     return 'false' since we don't know.
     * 0  - success (can suspend).       return 'false' since not unavailable.
     * 1  - success (cannot suspend).    return 'true' since unavailable.
     */
    if (WEXITSTATUS (status) == 1 )
        return TRUE;
    else
        return FALSE;
#else
    return FALSE; /* return 'false' since we don't know. */
#endif
}

/* Update the text label in the battery low dialog.
 */
static void
battery_low_update_text (ProgressData  *battstat,
                         BatteryStatus *info)
{
    const char *suggest;
    gchar *remaining, *new_label;
    GtkRequisition size;

    /* If we're not displaying the dialog then don't update it. */
    if (battstat->battery_low_label == NULL ||
        battstat->battery_low_dialog == NULL)
        return;

    gtk_widget_get_preferred_size (GTK_WIDGET (battstat->battery_low_label), NULL, &size);

    /* If the label has never been set before, the width will be 0.  If it
       has been set before (width > 0) then we want to keep the size of
       the old widget (to keep the dialog from changing sizes) so we set it
       explicitly here.
     */
    if (size.width > 0)
        gtk_widget_set_size_request (GTK_WIDGET (battstat->battery_low_label),
                                     size.width, size.height);

    if (info->minutes < 0 && !info->on_ac_power)
    {
        /* we don't know the remaining time */
        remaining = g_strdup_printf (_("You have %d%% of your total battery "
                                       "capacity remaining."), info->percent);
    }
    else
    {
      remaining = g_strdup_printf (ngettext ("You have %d minute of battery power "
                                             "remaining (%d%% of the total capacity).",
                                             "You have %d minutes of battery power "
                                             "remaining (%d%% of the total capacity).",
                                             info->minutes),
                                   info->minutes,
                                   info->percent);
    }

    if (is_suspend_unavailable ())
    /* TRANSLATORS: this is a list, it is left as a single string
     * to allow you to make it appear like a list would in your
     * locale.  This is if the laptop does not support suspend. */
        suggest = _("To avoid losing your work:\n"
                    " \xE2\x80\xA2 plug your laptop into external power, or\n"
                    " \xE2\x80\xA2 save open documents and shut your laptop down.");
    else
    /* TRANSLATORS: this is a list, it is left as a single string
     * to allow you to make it appear like a list would in your
     * locale.  This is if the laptop supports suspend. */
        suggest = _("To avoid losing your work:\n"
                    " \xE2\x80\xA2 suspend your laptop to save power,\n"
                    " \xE2\x80\xA2 plug your laptop into external power, or\n"
                    " \xE2\x80\xA2 save open documents and shut your laptop down.");

    new_label = g_strdup_printf ("<span weight=\"bold\" size=\"larger\">%s</span>\n\n%s\n\n%s",
                                 _("Your battery is running low"), remaining, suggest);

    gtk_label_set_markup (battstat->battery_low_label, new_label);
    g_free (remaining);
    g_free (new_label);
}

/* Show a dialog notifying the user that their battery is running low.
 */
static void
battery_low_dialog (ProgressData  *battery,
                    BatteryStatus *info)
{
    GtkWidget *hbox, *image, *label;
    GtkWidget *vbox;
    cairo_surface_t *surface;

    /* If the dialog is already displayed then don't display it again. */
    if (battery->battery_low_dialog != NULL)
        return;

    battery->battery_low_dialog =
            gtk_dialog_new_with_buttons (_("Battery Notice"),
                                         NULL,
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "gtk-ok",
                                         GTK_RESPONSE_ACCEPT,
                                         NULL);

    gtk_dialog_set_default_response (GTK_DIALOG (battery->battery_low_dialog),
                                     GTK_RESPONSE_ACCEPT);

    g_signal_connect_swapped (battery->battery_low_dialog, "response",
                              G_CALLBACK (battery_low_dialog_destroy),
                              battery);

    gtk_container_set_border_width (GTK_CONTAINER (battery->battery_low_dialog),
                                    6);

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width (GTK_CONTAINER (hbox), 6);
    surface = gtk_icon_theme_load_surface (gtk_icon_theme_get_default (),
                                           "battery",
                                           48,
                                           gtk_widget_get_scale_factor (GTK_WIDGET (hbox)),
                                           NULL,
                                           GTK_ICON_LOOKUP_USE_BUILTIN,
                                           NULL);

    image = gtk_image_new_from_surface (surface);
    cairo_surface_destroy (surface);
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start (GTK_BOX (hbox), vbox, FALSE, FALSE, 6);
    gtk_box_pack_start (GTK_BOX (vbox), image, FALSE, FALSE, 0);
    label = gtk_label_new ("");
    battery->battery_low_label = GTK_LABEL (label);
    gtk_label_set_line_wrap (battery->battery_low_label, TRUE);
    gtk_label_set_selectable (battery->battery_low_label, TRUE);
    gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 6);
    gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (battery->battery_low_dialog))),
                       hbox);

    gtk_window_set_keep_above (GTK_WINDOW (battery->battery_low_dialog), TRUE);
    gtk_window_stick (GTK_WINDOW (battery->battery_low_dialog));
    gtk_window_set_focus_on_map (GTK_WINDOW (battery->battery_low_dialog),
                                 FALSE);
    gtk_window_set_skip_pager_hint (GTK_WINDOW (battery->battery_low_dialog),
                                    TRUE);

    battery_low_update_text (battery, info);

    gtk_window_set_position (GTK_WINDOW (battery->battery_low_dialog),
                             GTK_WIN_POS_CENTER);
    gtk_widget_show_all (battery->battery_low_dialog);
}

/* Update the text of the tooltip from the provided info.
 */
static void
update_tooltip (ProgressData  *battstat,
                BatteryStatus *info)
{
    gchar *powerstring;
    gchar *remaining;
    gchar *tiptext;

    if (info->present)
    {
        if (info->on_ac_power)
            powerstring = AC_POWER_STRING;
        else
            powerstring = DC_POWER_STRING;

        remaining = get_remaining (info);

        tiptext = g_strdup_printf ("%s\n%s", powerstring, remaining);
        g_free (remaining);
    }
    else
    {
        if (info->on_ac_power)
            tiptext = g_strdup_printf ("%s\n%s", AC_POWER_STRING,
                                       _("No battery present"));
        else
            tiptext = g_strdup_printf ("%s\n%s", DC_POWER_STRING,
                                       _("Battery status unknown"));
    }

    gtk_widget_set_tooltip_text (battstat->applet, tiptext);
    g_free (tiptext);
}

/* Update the text label that either shows the percentage of time left.
 */
static void
update_percent_label (ProgressData  *battstat,
                      BatteryStatus *info)
{
    gchar *new_label;

    if (info->present && battstat->showtext == APPLET_SHOW_PERCENT)
      new_label = g_strdup_printf ("%d%%", info->percent);
    else if (info->present && battstat->showtext == APPLET_SHOW_TIME)
    {
        /* Fully charged or unknown (-1) time remaining display none */
        if ((info->on_ac_power && info->percent == 100) || info->minutes < 0)
            new_label = g_strdup ("");
        else
        {
            int time;
            time = info->minutes;
            new_label = g_strdup_printf ("%d:%02d", time/60, time%60);
        }
    }
    else
        new_label = g_strdup (_("N/A"));

    gtk_label_set_text (GTK_LABEL (battstat->percent), new_label);
    g_free (new_label);
}

/* Determine what status icon we ought to be displaying and change the
   status icon to display it if it is different from what we are currently
   showing.
 */
static void
possibly_update_status_icon (ProgressData  *battstat,
                             BatteryStatus *info )
{
    GtkIconTheme *theme;
    cairo_surface_t *surface;
    gint icon_size, icon_scale;
    gchar *icon_name;
    int batt_life;

    batt_life = !battstat->red_value_is_time ? info->percent : info->minutes;

    if (batt_life <= battstat->red_val)
    {
        if (info->charging)
            icon_name = "battery-caution-charging";
        else
            icon_name = "battery-caution";
    }
    else if (batt_life <= battstat->orange_val)
    {
        if (info->charging)
            icon_name = "battery-low-charging";
        else
            icon_name = "battery-low";
    }
    else if (batt_life <= battstat->yellow_val)
    {
        if (info->charging)
            icon_name = "battery-good-charging";
        else
            icon_name = "battery-good";
    }
    else if (info->on_ac_power)
    {
        if (info->charging)
            icon_name = "battery-full-charging";
        else
            icon_name = "battery-full-charged";
    }
    else
    {
        icon_name = "battery-full";
    }

    theme = gtk_icon_theme_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (battstat->applet)));

    icon_size = mate_panel_applet_get_size (MATE_PANEL_APPLET (battstat->applet));
    icon_scale = gtk_widget_get_scale_factor (GTK_WIDGET (battstat->applet));

    surface = gtk_icon_theme_load_surface (theme, icon_name,
                                           icon_size,
                                           icon_scale,
                                           NULL, 0, NULL);

    gtk_image_set_from_surface (GTK_IMAGE (battstat->status),
                                surface);
    cairo_surface_destroy (surface);
}

/* Gets called as a gtk_timeout once per second.  Checks for updates and
   makes any changes as appropriate.
 */
static gboolean
check_for_updates (gpointer data)
{
    ProgressData *battstat = data;
    BatteryStatus info;
    const char *err;

    if (DEBUG) g_print ("check_for_updates ()\n");

    if ((err = power_management_getinfo (&info)))
        battstat_error_dialog (battstat->applet, err);

    if (!event_driven)
    {
        int timeout;

      /* if on AC and not event driven scale back the polls to once every 10 */
      if (info.on_ac_power)
          timeout = 10;
      else
          timeout = 2;

      if (timeout != battstat->timeout)
      {
          battstat->timeout = timeout;

          if (battstat->timeout_id)
              g_source_remove (battstat->timeout_id);

          battstat->timeout_id = g_timeout_add_seconds (battstat->timeout,
                                                        check_for_updates,
                                                        battstat);
      }
    }

    possibly_update_status_icon (battstat, &info);

    if (!info.on_ac_power &&
        battstat->last_batt_life != 1000 &&
        (
            /* if percentage drops below red_val */
            (!battstat->red_value_is_time &&
             battstat->last_batt_life > battstat->red_val &&
             info.percent <= battstat->red_val) ||
            /* if time drops below red_val */
            (battstat->red_value_is_time &&
             battstat->last_minutes > battstat->red_val &&
             info.minutes <= battstat->red_val)
        )
        && info.present)
    {
        /* Warn that battery dropped below red_val */
        if (battstat->lowbattnotification)
        {
            battery_low_dialog (battstat, &info);

            if (battstat->beep)
                gdk_display_beep (gdk_display_get_default ());
        }
    }

    if (battstat->last_charging &&
        battstat->last_acline_status &&
        battstat->last_acline_status!=1000 &&
        !info.charging &&
        info.on_ac_power &&
        info.present &&
        info.percent > 99)
    {
        /* Inform that battery now fully charged */
        if (battstat->fullbattnot)
        {
            battery_full_dialog (battstat->applet);

            if (battstat->beep)
                gdk_display_beep (gdk_display_get_default ());
        }
    }

    /* If the warning dialog is displayed and we just got plugged in then
       stop displaying it.
     */
    if (battstat->battery_low_dialog && info.on_ac_power)
        battery_low_dialog_destroy (battstat);

    if (info.on_ac_power != battstat->last_acline_status ||
        info.percent != battstat->last_batt_life ||
        info.minutes != battstat->last_minutes ||
        info.charging != battstat->last_charging)
    {
        /* Update the tooltip */
        update_tooltip (battstat, &info);

        /* If the warning dialog box is currently displayed, update that too. */
        if (battstat->battery_low_dialog != NULL)
            battery_low_update_text (battstat, &info);
    }

    if ((battstat->showtext == APPLET_SHOW_PERCENT &&
         battstat->last_batt_life != info.percent) ||
        (battstat->showtext == APPLET_SHOW_TIME &&
         battstat->last_minutes != info.minutes) ||
        battstat->last_acline_status != info.on_ac_power ||
        battstat->last_present != info.present ||
        battstat->refresh_label) /* set by properties dialog */
    {
        /* Update the label */
        update_percent_label (battstat, &info);

        /* done */
        battstat->refresh_label = FALSE;
    }

    battstat->last_charging = info.charging;
    battstat->last_batt_life = info.percent;
    battstat->last_minutes = info.minutes;
    battstat->last_acline_status = info.on_ac_power;
    battstat->last_present = info.present;

    return TRUE;
}

/* Gets called when the user removes the applet from the panel.  Clean up
   all instance-specific data and call the global teardown function to
   decrease our applet count (and possibly perform global cleanup)
 */
static void
destroy_applet (GtkWidget    *widget,
                ProgressData *battstat)
{
    if (DEBUG) g_print ("destroy_applet ()\n");

    if (battstat->prop_win)
        gtk_widget_destroy (GTK_WIDGET (battstat->prop_win));

    if (battstat->battery_low_dialog)
        battery_low_dialog_destroy (battstat);

    if (battstat->timeout_id)
        g_source_remove (battstat->timeout_id);

    g_object_unref (G_OBJECT (battstat->status));
    g_object_unref (G_OBJECT (battstat->percent));
    g_object_unref (battstat->settings);

    static_global_teardown (battstat);

    g_free (battstat);
}

/* Common function invoked by the 'Help' context menu item and the 'Help'
 * button in the preferences dialog.
 */
void
battstat_show_help (ProgressData *battstat,
                    const char   *section)
{
    GError *error = NULL;
    char *uri;

    if (section)
        uri = g_strdup_printf ("help:mate-battstat/%s", section);
    else
        uri = g_strdup ("help:mate-battstat");

    gtk_show_uri_on_window (NULL,
                            uri,
                            gtk_get_current_event_time (),
                            &error);

    g_free (uri);

    if (error)
    {
        char *message;

        message = g_strdup_printf (_("There was an error displaying help: %s"),
                                   error->message );
        battstat_error_dialog (battstat->applet, message);
        g_error_free (error);
        g_free (message);
    }
}

/* Called when the user selects the 'help' menu item.
 */
static void
help_cb (GtkAction    *action,
         ProgressData *battstat)
{
    battstat_show_help (battstat, NULL);
}

/* Called when the user selects the 'about' menu item.
 */
static void
about_cb (GtkAction *action, ProgressData *battstat)
{
    const gchar *authors[] = {
        "J\xC3\xB6rgen Pehrson <jp@spektr.eu.org>",
        "Lennart Poettering <lennart@poettering.de> (Linux ACPI support)",
        "Seth Nickell <snickell@stanford.edu> (GNOME2 port)",
        "Davyd Madeley <davyd@madeley.id.au>",
        "Ryan Lortie <desrt@desrt.ca>",
        "Joe Marcus Clarke <marcus@FreeBSD.org> (FreeBSD ACPI support)",
        NULL
    };

    const gchar *documenters[] = {
        "J\xC3\xB6rgen Pehrson <jp@spektr.eu.org>",
        "Trevor Curtis <tcurtis@somaradio.ca>",
        "Davyd Madeley <davyd@madeley.id.au>",
        N_("MATE Documentation Team"),
        NULL
    };

    char *comments = g_strdup_printf ("%s\n\n%s",
                                      _("This utility shows the status of your laptop battery."),
                                      power_management_using_upower () ?
                                      /* true */ _("upower backend enabled.") :
                                      /* false */ _("Legacy backend enabled."));

#ifdef ENABLE_NLS
    const char **p;
    for (p = documenters; *p; ++p)
        *p = _(*p);
#endif

    gtk_show_about_dialog (NULL,
                           "title",              _("About Battery Charge Monitor"),
                           "version",            VERSION,
                           "copyright",          _("Copyright \xc2\xa9 2000 The Gnulix Society\n"
                                                   "Copyright \xc2\xa9 2002-2005 Free Software Foundation and others\n"
                                                   "Copyright \xc2\xa9 2012-2021 MATE developers"),
                           "comments",           comments,
                           "authors",            authors,
                           "documenters",        documenters,
                           "translator-credits", _("translator-credits"),
                           "logo-icon-name",     "battery",
                           NULL);

    g_free (comments);
}

/* Rotate text on side panels.  Called on initial startup and when the
 * orientation changes (ie: the panel we were on moved or we moved to
 * another panel).
 */
static void
setup_text_orientation (ProgressData *battstat)
{
    if (battstat->orienttype == MATE_PANEL_APPLET_ORIENT_RIGHT)
        gtk_label_set_angle (GTK_LABEL (battstat->percent), 90);
    else if (battstat->orienttype == MATE_PANEL_APPLET_ORIENT_LEFT)
        gtk_label_set_angle (GTK_LABEL (battstat->percent), 270);
    else
        gtk_label_set_angle (GTK_LABEL (battstat->percent), 0);
}

/* This signal is delivered by the panel when the orientation of the applet
   has changed.  This is either because the applet has just been created,
   has just been moved to a new panel or the panel that the applet was on
   has changed orientation.
*/
static void
change_orient (MatePanelApplet       *applet,
               MatePanelAppletOrient  orient,
               ProgressData          *battstat)
{
    if (DEBUG) g_print ("change_orient ()\n");

    /* Ignore the update if we already know. */
    if (orient != battstat->orienttype)
    {
        battstat->orienttype = orient;

        /* The applet changing orientation very likely involves the layout
           being changed to better fit the new shape of the panel.
        */
        setup_text_orientation (battstat);
        reconfigure_layout (battstat);
    }
}

/* This is delivered when our size has changed.  This happens when the applet
   is just created or if the size of the panel has changed.
*/
static void
size_allocate (MatePanelApplet *applet,
               GtkAllocation   *allocation,
               ProgressData    *battstat)
{
    if (DEBUG) g_print ("applet_change_pixel_size ()\n");

    /* Ignore the update if we already know. */
    if (battstat->width == allocation->width &&
        battstat->height == allocation->height)
        return;

    battstat->width = allocation->width;
    battstat->height = allocation->height;

    /* The applet changing size could result in the layout changing. */
    reconfigure_layout (battstat);
}

/* Get our settings out of gsettings.
 */
static void
load_preferences (ProgressData *battstat)
{
    GSettings *settings = battstat->settings;

    if (DEBUG) g_print ("load_preferences ()\n");

    battstat->red_val = g_settings_get_int (settings, "red-value");
    battstat->red_val = MIN (battstat->red_val, 100);
    battstat->red_value_is_time = g_settings_get_boolean (settings, "red-value-is-time");

    /* automatically calculate orangle and yellow values from the red value */
    battstat->orange_val = battstat->red_val * ORANGE_MULTIPLIER;
    battstat->orange_val = MIN (battstat->orange_val, 100);

    battstat->yellow_val = battstat->red_val * YELLOW_MULTIPLIER;
    battstat->yellow_val = MIN (battstat->yellow_val, 100);

    battstat->lowbattnotification = g_settings_get_boolean (settings, "low-battery-notification");
    battstat->fullbattnot = g_settings_get_boolean (settings, "full-battery-notification");
    battstat->beep = g_settings_get_boolean (settings, "beep");
    battstat->showtext = g_settings_get_int (settings, "show-text");
}

/* Convenience function to attach a child widget to a GtkGrid in the
   position indicated by 'loc'.  This is very special-purpose for 3x3
   gridss and only supports positions that are used in this applet.
 */
static void
grid_layout_attach (GtkGrid        *grid,
                    LayoutLocation  loc,
                    GtkWidget      *child)
{
    switch (loc)
    {
        case LAYOUT_LONG:
            gtk_grid_attach (grid, child, 1, 0, 1, 2);
            break;

        case LAYOUT_TOPLEFT:
            gtk_grid_attach (grid, child, 0, 0, 1, 1);
            break;

        case LAYOUT_TOP:
            gtk_grid_attach (grid, child, 1, 0, 1, 1);
            break;

        case LAYOUT_LEFT:
            gtk_grid_attach (grid, child, 0, 1, 1, 1);
            break;

        case LAYOUT_CENTRE:
            gtk_grid_attach (grid, child, 1, 1, 1, 1);
            break;

        case LAYOUT_RIGHT:
            gtk_grid_attach (grid, child, 2, 1, 1, 1);
            break;

        case LAYOUT_BOTTOM:
            gtk_grid_attach (grid, child, 1, 2, 1, 1);
            break;

        default:
            break;
    }
}

/* The layout has (maybe) changed.  Calculate what layout we ought to be
   using and update some things if anything has changed.  This is called
   from size/orientation change callbacks and from the preferences dialog
   when elements get added or removed.
 */
void
reconfigure_layout (ProgressData *battstat)
{
    LayoutConfiguration c;

    /* Default to no elements being displayed. */
    c.status = c.text = LAYOUT_NONE;

    switch (battstat->orienttype)
    {
      case MATE_PANEL_APPLET_ORIENT_UP:
      case MATE_PANEL_APPLET_ORIENT_DOWN:
          /* Stack horizontally for top and bottom panels. */
          c.status = LAYOUT_LEFT;
          if (battstat->showtext)
              c.text = LAYOUT_RIGHT;
          break;

      case MATE_PANEL_APPLET_ORIENT_LEFT:
      case MATE_PANEL_APPLET_ORIENT_RIGHT:
          /* Stack vertically for left and right panels. */
          c.status = LAYOUT_TOP;
          if (battstat->showtext)
              c.text = LAYOUT_BOTTOM;
          break;
    }

    if (memcmp (&c, &battstat->layout, sizeof (LayoutConfiguration)))
    {
        /* Something in the layout has changed.  Rebuild. */

        /* Start by removing any elements in the grid from the grid. */
        if (battstat->layout.text)
            gtk_container_remove (GTK_CONTAINER (battstat->grid),
                                  battstat->percent);
        if (battstat->layout.status)
            gtk_container_remove (GTK_CONTAINER (battstat->grid),
                                  battstat->status);

        /* Attach the elements to their new locations. */
        grid_layout_attach (GTK_GRID (battstat->grid),
                            c.status, battstat->status);
        grid_layout_attach (GTK_GRID (battstat->grid),
                            c.text, battstat->percent);

        gtk_widget_show_all (battstat->applet);
    }

    battstat->layout = c;

    /* Check for generic updates. This is required, for example, to make sure
       the text label is immediately updated to show the time remaining or
       percentage.
    */
    check_for_updates (battstat);
}

/* Allocate the widgets for the applet and connect our signals.
 */
static gint
create_layout (ProgressData *battstat)
{
    if (DEBUG) g_print ("create_layout ()\n");

    /* Allocate the four widgets that we need. */
    battstat->grid = gtk_grid_new ();
    battstat->percent = gtk_label_new ("");
    battstat->status = gtk_image_new ();

    /* When you first get a pointer to a newly created GtkWidget it has one
       'floating' reference.  When you first add this widget to a container
       the container adds a real reference and removes the floating reference
       if one exists.  Since we insert/remove these widgets from the table
       when our layout is reconfigured, we need to keep our own 'real'
       reference to each widget.  This adds a real reference to each widget
       and "sinks" the floating reference.
    */
    g_object_ref (battstat->status);
    g_object_ref (battstat->percent);
    g_object_ref_sink (G_OBJECT (battstat->status));
    g_object_ref_sink (G_OBJECT (battstat->percent));

    /* Let reconfigure_layout know that the grid is currently empty. */
    battstat->layout.status = LAYOUT_NONE;
    battstat->layout.text = LAYOUT_NONE;

    /* Put the grid directly inside the applet and show everything. */
    gtk_widget_set_halign (battstat->grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (battstat->grid, GTK_ALIGN_CENTER);
    gtk_container_add (GTK_CONTAINER (battstat->applet), battstat->grid);
    gtk_widget_show_all (battstat->applet);

    /* Attach all sorts of signals to the applet. */
    g_signal_connect (battstat->applet, "destroy",
                      G_CALLBACK (destroy_applet),
                      battstat);

    g_signal_connect (battstat->applet, "change_orient",
                      G_CALLBACK (change_orient),
                      battstat);

    g_signal_connect (battstat->applet, "size_allocate",
                      G_CALLBACK (size_allocate),
                      battstat);

    return FALSE;
}

void
prop_cb (GtkAction    *action,
         ProgressData *battstat)
{
    if (battstat->prop_win) {
        gtk_window_set_screen (GTK_WINDOW (battstat->prop_win),
                               gtk_widget_get_screen (battstat->applet));
        gtk_window_present (GTK_WINDOW (battstat->prop_win));
    } else {
        battstat->prop_win = battstat_preferences_new (battstat);
        gtk_widget_show_all (GTK_WIDGET (battstat->prop_win));
    }
}

/* Called by the factory to fill in the fields for the applet.
 */
static gboolean
battstat_applet_fill (MatePanelApplet *applet)
{
    ProgressData *battstat;
    AtkObject *atk_widget;
    GtkActionGroup *action_group;
    const char *err;

    if (DEBUG) g_print ("main ()\n");

    g_set_application_name (_("Battery Charge Monitor"));

    gtk_window_set_default_icon_name ("battery");

    mate_panel_applet_set_flags (applet,
                                 MATE_PANEL_APPLET_EXPAND_MINOR);

    battstat = g_new0 (ProgressData, 1);
    battstat->settings = mate_panel_applet_settings_new (applet,
                                                         BATTSTAT_SCHEMA);

    /* Some starting values... */
    battstat->applet = GTK_WIDGET (applet);
    battstat->refresh_label = TRUE;
    battstat->last_batt_life = 1000;
    battstat->last_acline_status = 1000;
    battstat->last_charging = 1000;
    battstat->orienttype = mate_panel_applet_get_orient (applet);
    battstat->battery_low_dialog = NULL;
    battstat->battery_low_label = NULL;
    battstat->timeout = -1;
    battstat->timeout_id = 0;

    /* The first received size_allocate event will cause a reconfigure. */
    battstat->height = -1;
    battstat->width = -1;

    load_preferences (battstat);
    create_layout (battstat);
    setup_text_orientation (battstat);

    action_group = gtk_action_group_new ("Battstat Applet Actions");
    gtk_action_group_set_translation_domain (action_group,
                                             GETTEXT_PACKAGE);
    gtk_action_group_add_actions (action_group,
                                  battstat_menu_actions,
                                  G_N_ELEMENTS (battstat_menu_actions),
                                  battstat);

    mate_panel_applet_setup_menu_from_resource (MATE_PANEL_APPLET (battstat->applet),
                                                BATTSTAT_RESOURCE_PATH "battstat-applet-menu.xml",
                                                action_group);

    if (mate_panel_applet_get_locked_down (MATE_PANEL_APPLET (battstat->applet))) {
        GtkAction *action;

        action = gtk_action_group_get_action (action_group,
                                              "BattstatProperties");
        gtk_action_set_visible (action, FALSE);
    }
    g_object_unref (action_group);

    atk_widget = gtk_widget_get_accessible (battstat->applet);
    if (GTK_IS_ACCESSIBLE (atk_widget)) {
        atk_object_set_name (atk_widget,
                             _("Battery Charge Monitor"));
        atk_object_set_description (atk_widget,
                                    _("Monitor a laptop's remaining power"));
    }

    if ((err = static_global_initialisation (battstat)))
        battstat_error_dialog (GTK_WIDGET (applet), err);

    return TRUE;
}

/* Boilerplate... */
static gboolean
battstat_applet_factory (MatePanelApplet *applet,
                         const gchar     *iid,
                         gpointer         data)
{
    gboolean retval = FALSE;

    if (!strcmp (iid, "BattstatApplet"))
        retval = battstat_applet_fill (applet);

    return retval;
}

MATE_PANEL_APPLET_OUT_PROCESS_FACTORY ("BattstatAppletFactory",
                                       PANEL_TYPE_APPLET,
                                       "battstat",
                                       battstat_applet_factory,
                                       NULL)

