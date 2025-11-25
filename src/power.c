/*============================================================================
Copyright (c) 2023-2025 Raspberry Pi
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
============================================================================*/

#include <locale.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <libudev.h>

#ifdef LXPLUG
#include "plugin.h"
#else
#include "lxutils.h"
#endif

#include "power.h"

/*----------------------------------------------------------------------------*/
/* Typedefs and macros                                                        */
/*----------------------------------------------------------------------------*/

#define POWER_PATH "/proc/device-tree/chosen/power/"
#define WARN_FILE  "/proc/device-tree/chosen/user-warnings"

/* Reasons to show the icon */
#define ICON_LOW_VOLTAGE    0x01
#define ICON_OVER_CURRENT   0x02
#define ICON_BROWNOUT       0x04

/*----------------------------------------------------------------------------*/
/* Global data                                                                */
/*----------------------------------------------------------------------------*/

conf_table_t conf_table[1] = {
    {CONF_TYPE_NONE, NULL, NULL, NULL}
};

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static void check_psu (PowerPlugin *pt);
static void check_brownout (PowerPlugin *pt);
static void check_user_warnings (PowerPlugin *pt);
static char *get_string (char *cmd);
static void check_memres (PowerPlugin *pt);
static gboolean startup_checks (gpointer data);
static gboolean cb_overcurrent_fd (gint, GIOCondition, gpointer data);
static gboolean cb_lowvoltage_fd (gint, GIOCondition, gpointer data);
static void update_icon (PowerPlugin *pt);
static void show_info (GtkWidget *, gpointer);
static void power_button_clicked (GtkWidget *, PowerPlugin *pt);

/*----------------------------------------------------------------------------*/
/* Function definitions                                                       */
/*----------------------------------------------------------------------------*/

/* Tests */

static void check_psu (PowerPlugin *pt)
{
    if (system ("raspi-config nonint is_cmfive") == 0) return;

    FILE *fp = fopen (POWER_PATH "max_current", "rb");
    int val;

    if (fp)
    {
        unsigned char *cptr = (unsigned char *) &val;
        // you're kidding, right?
        for (int i = 3; i >= 0; i--) cptr[i] = fgetc (fp);
        if (val < 5000) wrap_notify (pt->panel, _("This power supply is not capable of supplying 5A\nPower to peripherals will be restricted"));
        fclose (fp);
    }
}

static void check_brownout (PowerPlugin *pt)
{
    FILE *fp = fopen (POWER_PATH "power_reset", "rb");
    int val;

    if (fp)
    {
        unsigned char *cptr = (unsigned char *) &val;
        for (int i = 3; i >= 0; i--) cptr[i] = fgetc (fp);
        if (val & 0x02)
        {
            wrap_critical (pt->panel, _("Reset due to low power event\nPlease check your power supply"));
            pt->show_icon |= ICON_BROWNOUT;
            update_icon (pt);
        }
        fclose (fp);
    }
}

static void check_user_warnings (PowerPlugin *pt)
{
    if (!access (WARN_FILE, F_OK))
    {
        FILE *fp = fopen (WARN_FILE, "rb");
        if (fp)
        {
            char *buf = NULL;
            size_t siz = 0;
            while (getline (&buf, &siz, fp) != -1)
                wrap_notify (pt->panel, g_strstrip (buf));
            free (buf);
            fclose (fp);
        }
    }
}

static char *get_string (char *cmd)
{
    char *line = NULL, *res = NULL;
    size_t len = 0;
    FILE *fp = popen (cmd, "r");

    if (fp == NULL) return NULL;
    if (getline (&line, &len, fp) > 0)
    {
        res = line;
        while (*res)
        {
            if (g_ascii_isspace (*res)) *res = 0;
            res++;
        }
        res = g_strdup (line);
    }
    pclose (fp);
    g_free (line);
    return res;
}

#define MEM_WARN_THRESHOLD 2048
#define RES_HEIGHT_THRESHOLD 1200

static void check_memres (PowerPlugin *pt)
{
    char *res;
    int mem, width, height, max_h = 0;

    res = get_string ("vcgencmd get_config total_mem | cut -d = -f 2");
    if (!res) return;
    if (sscanf (res, "%d", &mem) != 1) return;
    g_free (res);
    if (mem < 256 || mem > MEM_WARN_THRESHOLD) return;

    res = get_string ("wlr-randr | sed -n '/^HDMI-A-1/,/Position/{/current/p}' | sed 's/ //g' | sed 's/px.*//'");
    if (res)
    {
        if (sscanf (res, "%dx%d", &width, &height) == 2)
            if (height > max_h) max_h = height;
        g_free (res);
    }

    res = get_string ("wlr-randr | sed -n '/^HDMI-A-2/,/Position/{/current/p}' | sed 's/ //g' | sed 's/px.*//'");
    if (res)
    {
        if (sscanf (res, "%dx%d", &width, &height) == 2)
            if (height > max_h) max_h = height;
        g_free (res);
    }

    if (max_h > RES_HEIGHT_THRESHOLD)
        wrap_notify (pt->panel, _("High display resolution is using large amounts of memory.\nConsider reducing screen resolution."));
}

/* Monitoring callbacks */

static gboolean startup_checks (gpointer data)
{
    PowerPlugin *pt = (PowerPlugin *) data;

    check_psu (pt);
    check_brownout (pt);
    check_memres (pt);
    check_user_warnings (pt);

    pt->startup_id = 0;
    return G_SOURCE_REMOVE;
}

static gboolean cb_overcurrent_fd (gint, GIOCondition, gpointer data)
{
    PowerPlugin *pt = (PowerPlugin *) data;
    int val;
    struct udev_device *dev;
    FILE *fp;
    char *path;

    dev = udev_monitor_receive_device (pt->udev_mon_oc);
    if (dev)
    {
        if (!g_strcmp0 (udev_device_get_action (dev), "change"))
        {
            path = g_strdup_printf ("/sys/%s/disable", udev_device_get_property_value (dev, "OVER_CURRENT_PORT"));
            fp = fopen (path, "rb");
            if (fp)
            {
                if (fgetc (fp) == 0x31)
                {
                    if (sscanf (udev_device_get_property_value (dev, "OVER_CURRENT_COUNT"), "%d", &val) == 1 && val != pt->last_oc)
                    {
                        wrap_critical (pt->panel, _("USB overcurrent\nPlease check your connected USB devices"));
                        pt->show_icon |= ICON_OVER_CURRENT;
                        update_icon (pt);
                        pt->last_oc = val;
                    }
                }
                fclose (fp);
            }
            g_free (path);
        }
        udev_device_unref (dev);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean cb_lowvoltage_fd (gint, GIOCondition, gpointer data)
{
    PowerPlugin *pt = (PowerPlugin *) data;
    struct udev_device *dev;
    FILE *fp;
    char *path;

    dev = udev_monitor_receive_device (pt->udev_mon_lv);
    if (dev)
    {
        if (!g_strcmp0 (udev_device_get_action (dev), "change") && !strncmp (udev_device_get_sysname (dev), "hwmon", 5))
        {
            path = g_strdup_printf ("%s/in0_lcrit_alarm", udev_device_get_syspath (dev));
            fp = fopen (path, "rb");
            if (fp)
            {
                if (fgetc (fp) == 0x31)
                {
                    wrap_critical (pt->panel, _("Low voltage warning\nPlease check your power supply"));
                    pt->show_icon |= ICON_LOW_VOLTAGE;
                    update_icon (pt);
                }
                fclose (fp);
            }
            g_free (path);
        }
        udev_device_unref (dev);
    }

    return G_SOURCE_CONTINUE;
}

/* Update the icon to show current status */

static void update_icon (PowerPlugin *pt)
{
    char *tooltip;

    wrap_set_taskbar_icon (pt, pt->tray_icon, "under-volt");
    gtk_widget_set_sensitive (pt->plugin, pt->show_icon);

    if (!pt->show_icon) gtk_widget_hide (pt->plugin);
    else
    {
        gtk_widget_show_all (pt->plugin);
        tooltip = g_strconcat (pt->show_icon & ICON_LOW_VOLTAGE ? _("PSU low voltage detected\n") : "",
            pt->show_icon & ICON_OVER_CURRENT ? _("USB over current detected\n") : "",
            pt->show_icon & ICON_BROWNOUT ? _("Low power reset has occurred\n") : "", NULL);
        tooltip[strlen (tooltip) - 1] = 0;
        gtk_widget_set_tooltip_text (pt->tray_icon, tooltip);
        g_free (tooltip);
    }
}

static void show_info (GtkWidget *, gpointer)
{
    system ("x-www-browser https://rptl.io/rpi5-power-supply-info &");
}

/*----------------------------------------------------------------------------*/
/* wf-panel plugin functions                                                  */
/*----------------------------------------------------------------------------*/

/* Handler for button click */
static void power_button_clicked (GtkWidget *, PowerPlugin *pt)
{
    CHECK_LONGPRESS
    gtk_widget_show_all (pt->menu);
    wrap_show_menu (pt->plugin, pt->menu);
}

/* Handler for system config changed message from panel */
void power_update_display (PowerPlugin *pt)
{
    update_icon (pt);
}

void power_init (PowerPlugin *pt)
{
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

    /* Allocate icon as a child of top level */
    pt->tray_icon = gtk_image_new ();
    gtk_container_add (GTK_CONTAINER (pt->plugin), pt->tray_icon);

    /* Set up button */
    gtk_button_set_relief (GTK_BUTTON (pt->plugin), GTK_RELIEF_NONE);
#ifndef LXPLUG
    g_signal_connect (pt->plugin, "clicked", G_CALLBACK (power_button_clicked), pt);
#endif

    /* Set up variables */
    pt->show_icon = 0;
    pt->udev_mon_oc = NULL;
    pt->udev_mon_lv = NULL;
    pt->udev = NULL;
    pt->overcurrent_id = 0;
    pt->lowvoltage_id = 0;
    pt->startup_id = 0;

    pt->menu = gtk_menu_new ();
    GtkWidget *item = gtk_menu_item_new_with_label (_("Power Information..."));
    g_signal_connect (G_OBJECT (item), "activate", G_CALLBACK (show_info), NULL);
    gtk_menu_shell_append (GTK_MENU_SHELL (pt->menu), item);

    /* Start timed events to monitor low voltage warnings */
    if (is_pi ())
    {
        pt->last_oc = -1;
        pt->udev = udev_new ();

        /* Configure udev monitors */
        pt->udev_mon_oc = udev_monitor_new_from_netlink (pt->udev, "kernel");
        if (pt->udev_mon_oc)
        {
            udev_monitor_filter_add_match_subsystem_devtype (pt->udev_mon_oc, "usb", NULL);
            udev_monitor_enable_receiving (pt->udev_mon_oc);
            pt->overcurrent_id = g_unix_fd_add (udev_monitor_get_fd (pt->udev_mon_oc), G_IO_IN, cb_overcurrent_fd, pt);
        }

        pt->udev_mon_lv = udev_monitor_new_from_netlink (pt->udev, "kernel");
        if (pt->udev_mon_lv)
        {
            udev_monitor_filter_add_match_subsystem_devtype (pt->udev_mon_lv, "hwmon", NULL);
            udev_monitor_enable_receiving (pt->udev_mon_lv);
            pt->lowvoltage_id = g_unix_fd_add (udev_monitor_get_fd (pt->udev_mon_lv), G_IO_IN, cb_lowvoltage_fd, pt);
        }

        pt->startup_id = g_idle_add (startup_checks, pt);
    }
}

void power_destructor (gpointer user_data)
{
    PowerPlugin *pt = (PowerPlugin *) user_data;

    if (pt->overcurrent_id > 0) g_source_remove (pt->overcurrent_id);
    pt->overcurrent_id = 0;
    if (pt->lowvoltage_id > 0) g_source_remove (pt->lowvoltage_id);
    pt->lowvoltage_id = 0;
    if (pt->startup_id > 0) g_source_remove (pt->startup_id);
    pt->startup_id = 0;

    if (pt->udev_mon_oc) udev_monitor_unref (pt->udev_mon_oc);
    pt->udev_mon_oc = NULL;
    if (pt->udev_mon_lv) udev_monitor_unref (pt->udev_mon_lv);
    pt->udev_mon_lv = NULL;
    if (pt->udev) udev_unref (pt->udev);
    pt->udev = NULL;
    g_free (pt);
}

/*----------------------------------------------------------------------------*/
/* LXPanel plugin functions                                                   */
/*----------------------------------------------------------------------------*/
#ifdef LXPLUG

/* Constructor */
static GtkWidget *power_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    PowerPlugin *pt = g_new0 (PowerPlugin, 1);

    /* Allocate top level widget and set into plugin widget pointer. */
    pt->panel = panel;
    pt->settings = settings;
    pt->plugin = gtk_button_new ();
    lxpanel_plugin_set_data (pt->plugin, pt, power_destructor);

    power_init (pt);

    return pt->plugin;
}

/* Handler for button press */
static gboolean power_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *)
{
    PowerPlugin *pt = lxpanel_plugin_get_data (widget);
    if (event->button == 1)
    {
        power_button_clicked (widget, pt);
        return TRUE;
    }
    else return FALSE;
}

/* Handler for system config changed message from panel */
static void power_configuration_changed (LXPanel *, GtkWidget *plugin)
{
    PowerPlugin *pt = lxpanel_plugin_get_data (plugin);
    power_update_display (pt);
}

int module_lxpanel_gtk_version = 1;
char module_name[] = PLUGIN_NAME;

/* Plugin descriptor */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = PLUGIN_TITLE,
    .description = N_("Monitors system power"),
    .new_instance = power_constructor,
    .reconfigure = power_configuration_changed,
    .button_press_event = power_button_press_event,
    .gettext_package = GETTEXT_PACKAGE
};
#endif

/* End of file */
/*----------------------------------------------------------------------------*/
