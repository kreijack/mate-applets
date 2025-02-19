#include <config.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <mate-panel-applet.h>
#include <mate-panel-applet-gsettings.h>
#include <math.h>

#include "global.h"

/*
  Shifts data right

  data[i+1] = data[i]

  data[i] are int*, so we just move the pointer, not the data.
  But moving data loses data[n-1], so we save data[n-1] and reuse
  it as new data[0]. In fact, we rotate data[].

*/

static void
shift_right(LoadGraph *g)
{
    guint64 *last_data;
    gsize i;

    /* data[g->draw_width - 1] becomes data[0] */
    last_data = g->data[g->draw_width - 1];

    /* data[i+1] = data[i] */
    for (i = g->draw_width - 1; i != 0; --i)
      g->data[i] = g->data[i - 1];

    g->data[0] = last_data;
}

/* Redraws the backing pixmap for the load graph and updates the window */
static void
load_graph_draw (LoadGraph *g)
{
  guint i, j, k;
  cairo_t *cr;
  MultiloadApplet *multiload;

  multiload = g->multiload;

  /* we might get called before the configure event so that
   * g->disp->allocation may not have the correct size
   * (after the user resized the applet in the prop dialog). */

  if (!g->surface)
    g->surface = gdk_window_create_similar_surface (gtk_widget_get_window (g->disp),
                                                    CAIRO_CONTENT_COLOR,
                                                    (int) g->draw_width,
                                                    (int) g->draw_height);

  cr = cairo_create (g->surface);
  cairo_set_line_width (cr, 1.0);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

  /* all graphs except Load and Net go this path */
  switch (g->id) {

  /* This is for network graph */
  case graph_netload2: {
    guint64 maxnet = 1;
    guint64 segments = 1;
    guint64 net_threshold;
    guint   level = 0;
    double  ratio;
    double  spacing;

    for (i = 0; i < g->draw_width; i++)
    {
      g->pos [i] = g->draw_height - 1;
      if (g->data[i][3] > maxnet)
        maxnet = g->data[i][3];
    }
    //printf("max = %d ", maxnet);
    if (maxnet > multiload->net_threshold3) {
      net_threshold = multiload->net_threshold3;
      level = 3;
    }
    else
      if (maxnet > multiload->net_threshold2) {
        net_threshold = multiload->net_threshold2;
        level = 2;
      }
      else {
        net_threshold = multiload->net_threshold1;
        if (maxnet >= multiload->net_threshold1)
            level = 1;
      }

    //printf("level %d maxnet = %d ", level, maxnet);
    maxnet = maxnet/net_threshold;
    segments = MAX (maxnet+1,1);
    ratio = (double) g->draw_height / (double) (net_threshold*segments);
    //printf("segments %d ratio = %f t1=%ld t2=%ld t3=%ld t=%ld\n", segments, ratio, multiload->net_threshold1, multiload->net_threshold2, multiload->net_threshold3, multiload->net_threshold);

    for (j = 0; j < g->n-1; j++)
    {
      gdk_cairo_set_source_rgba (cr, &(g->colors [j]));

      for (i = 0; i < g->draw_width; i++)
      {
        double x = (double) (g->draw_width - i) - 0.5;
        cairo_move_to (cr, x, (double) g->pos[i] + 0.5);
        cairo_line_to (cr, x, (double) g->pos[i] - 0.5 - (((double) g->data [i][j] * ratio)));
        g->pos [i] -= (guint64) ((double) g->data [i][j] * ratio);
      }
      cairo_stroke (cr);
    }

    for (j = g->n-1; j < g->n; j++)
    {
      gdk_cairo_set_source_rgba (cr, &(g->colors [j]));
      for (i = 0; i < g->draw_width; i++)
      {
          double x = (double) (g->draw_width - i) - 0.5;
          cairo_move_to (cr, x, (double) g->pos[i] + 0.5);
          cairo_line_to (cr, x, 0.5);
      }
      cairo_stroke (cr);
    }

    /* draw grid lines if needed */
    gdk_cairo_set_source_rgba (cr, &(g->colors [4]));
    for (k = 0; k < segments -1; k++)
    {
      spacing = ((double) g->draw_height / (double) segments) * (k+1);
      cairo_move_to (cr, 0.5, spacing);
      cairo_line_to (cr, (double) g->draw_width - 0.5, spacing);
    }
    cairo_stroke (cr);
    /* draw indicator if needed */
    if (level > 0)
    {
      gdk_cairo_set_source_rgba (cr, &(g->colors [5]));
      for (k = 0; k< level; k++ )
        cairo_rectangle (cr,
                         0.5, (double) k * 2.0 * (double) g->draw_height / 5.0,
                         5.0, (double) g->draw_height / 5.0);
      cairo_fill(cr);
    }
    cairo_stroke (cr);
    break;
  }

  /* this is Load graph */
  case graph_loadavg: {
    double load;
    guint64 maxload = 1;
    for (i = 0; i < g->draw_width; i++)
    {
      g->pos [i] = g->draw_height - 1;
      /* find maximum value */
      if (g->data[i][0] > maxload)
        maxload = g->data[i][0];
    }
    load = ceil ((double) maxload / (double) g->draw_height) + 1.0;

    for (j = 0; j < g->n; j++)
    {
      gdk_cairo_set_source_rgba (cr, &(g->colors [j]));

      for (i = 0; i < g->draw_width; i++)
      {
        double x = (double) (g->draw_width - i) - 0.5;
        cairo_move_to (cr, x, (double) g->pos[i] + 0.5);
        if (j == 0)
        {
          cairo_line_to (cr, x, (double) g->pos[i] - (((double) g->data [i][j] - 0.5)/load));
        }
        else
        {
          cairo_line_to (cr, x, 0.5);
        }
        g->pos [i] -= (guint64) ((double) g->data [i][j] / load);
      }
      cairo_stroke (cr);
    }

    /* draw grid lines in Load graph if needed */
    gdk_cairo_set_source_rgba (cr, &(g->colors [2]));

    double spacing;
    for (k = 0; k < load - 1; k++)
    {
      spacing = ((double) g->draw_height/load) * (k+1);
      cairo_move_to (cr, 0.5, spacing);
      cairo_line_to (cr, (double) g->draw_width - 0.5, spacing);
    }

    cairo_stroke (cr);
    break;
  }

  default:
    for (i = 0; i < g->draw_width; i++)
      g->pos [i] = g->draw_height - 1;

    for (j = 0; j < g->n; j++)
    {
      gdk_cairo_set_source_rgba (cr, &(g->colors [j]));

      for (i = 0; i < g->draw_width; i++)
      {
        if (g->data [i][j] != 0)
        {
          double x = (double) (g->draw_width - i) - 0.5;
          cairo_move_to (cr, x, (double) g->pos[i] + 0.5);
          cairo_line_to (cr, x, (double) g->pos[i] - (double) g->data [i][j] - 0.5);
        }
        g->pos [i] -= g->data [i][j];
      }
      cairo_stroke (cr);
    }
  }

  gtk_widget_queue_draw (g->disp);

  cairo_destroy (cr);
}

/* Updates the load graph when the timeout expires */
static gboolean
load_graph_update (LoadGraph *g)
{
    if (g->data == NULL)
        return TRUE;

    shift_right(g);

    if (g->tooltip_update)
        multiload_applet_tooltip_update (g);

    g->get_data (g->draw_height, g->data [0], g);

    load_graph_draw (g);
    return TRUE;
}

void
load_graph_unalloc (LoadGraph *g)
{
    gsize i;

    if (!g->allocated)
        return;

    for (i = 0; i < g->draw_width; i++)
    {
        g_free (g->data [i]);
    }

    g_free (g->data);
    g_free (g->pos);

    g->pos = NULL;
    g->data = NULL;

    g->size = CLAMP (g_settings_get_uint (g->multiload->settings, GRAPH_SIZE_KEY),
                     GRAPH_SIZE_MIN,
                     GRAPH_SIZE_MAX);

    if (g->surface) {
        cairo_surface_destroy (g->surface);
        g->surface = NULL;
    }

    g->allocated = FALSE;
}

static void
load_graph_alloc (LoadGraph *g)
{
    gsize i;
    gsize data_size;

    if (g->allocated)
        return;

    g->data = g_new0 (guint64 *, g->draw_width);
    g->pos = g_new0 (guint64, g->draw_width);

    data_size = sizeof (guint64) * g->n;

    for (i = 0; i < g->draw_width; i++) {
        g->data [i] = g_malloc0 (data_size);
    }

    g->allocated = TRUE;
}

static gint
load_graph_configure (GtkWidget *widget, GdkEventConfigure *event,
                      gpointer data_ptr)
{
    GtkAllocation allocation;
    LoadGraph *c = (LoadGraph *) data_ptr;

    load_graph_unalloc (c);

    gtk_widget_get_allocation (c->disp, &allocation);

    c->draw_width = (gsize) allocation.width;
    c->draw_height = (guint64) allocation.height;
    c->draw_width = MAX (c->draw_width, 1);
    c->draw_height = MAX (c->draw_height, 1);

    load_graph_alloc (c);

    if (!c->surface)
        c->surface = gdk_window_create_similar_surface (gtk_widget_get_window (c->disp),
                                                        CAIRO_CONTENT_COLOR,
                                                        (int) c->draw_width,
                                                        (int) c->draw_height);
    gtk_widget_queue_draw (widget);

    return TRUE;
}

static gint
load_graph_expose (GtkWidget *widget,
                   cairo_t *cr,
                   gpointer data_ptr)
{
    LoadGraph *g = (LoadGraph *) data_ptr;

    cairo_set_source_surface (cr, g->surface, 0, 0);
    cairo_paint (cr);

    return FALSE;
}

static void
load_graph_destroy (GtkWidget *widget, gpointer data_ptr)
{
    LoadGraph *g = (LoadGraph *) data_ptr;

    load_graph_stop (g);

    gtk_widget_destroy(widget);
}

static gboolean
load_graph_clicked (GtkWidget *widget, GdkEventButton *event, LoadGraph *load)
{
    load->multiload->last_clicked = load->id;

    return FALSE;
}

static gboolean
load_graph_enter_cb(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    LoadGraph *graph;
    graph = (LoadGraph *)data;

    graph->tooltip_update = TRUE;
    multiload_applet_tooltip_update(graph);

    return TRUE;
}

static gboolean
load_graph_leave_cb(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    LoadGraph *graph;
    graph = (LoadGraph *)data;

    graph->tooltip_update = FALSE;

    return TRUE;
}

static void
load_graph_load_config (LoadGraph *g)
{
    gchar *name, *temp;
    guint i;

    if (!g->colors)
        g->colors = g_new0(GdkRGBA, g->n);

    for (i = 0; i < g->n; i++)
    {
        name = g_strdup_printf ("%s-color%u", g->name, i);
        temp = g_settings_get_string(g->multiload->settings, name);
        if (!temp)
            temp = g_strdup ("#000000");
        gdk_rgba_parse(&(g->colors[i]), temp);
        g_free(temp);
        g_free(name);
    }
}

LoadGraph *
load_graph_new (MultiloadApplet *ma, guint n, const gchar *label,
                gint id, guint speed, guint size, gboolean visible,
                const gchar *name, LoadGraphDataFunc get_data)
{
    LoadGraph *g;
    MatePanelAppletOrient orient;

    g = g_new0 (LoadGraph, 1);
    g->visible = visible;
    g->name = name;
    g->n = n;
    g->id = id;
    g->speed = speed;
    g->size = size;
    g->pixel_size = mate_panel_applet_get_size (ma->applet);
    g->tooltip_update = FALSE;
    g->multiload = ma;

    g->main_widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    g->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    orient = mate_panel_applet_get_orient (g->multiload->applet);
    switch (orient)
    {
    case MATE_PANEL_APPLET_ORIENT_UP:
    case MATE_PANEL_APPLET_ORIENT_DOWN:
    {
        g->orient = FALSE;
        break;
    }
    case MATE_PANEL_APPLET_ORIENT_LEFT:
    case MATE_PANEL_APPLET_ORIENT_RIGHT:
    {
        g->orient = TRUE;
        break;
    }
    default:
        g_assert_not_reached ();
    }

    g->frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (g->frame), GTK_SHADOW_IN);
    gtk_container_add (GTK_CONTAINER (g->frame), g->box);
    gtk_box_pack_start (GTK_BOX (g->main_widget), g->frame, TRUE, TRUE, 0);

    load_graph_load_config (g);

    g->get_data = get_data;

    g->timer_index = -1;

    if (g->orient)
        gtk_widget_set_size_request (g->main_widget, -1, (gint) g->size);
    else
        gtk_widget_set_size_request (g->main_widget, (gint) g->size, -1);

    g->disp = gtk_drawing_area_new ();
    gtk_widget_set_events (g->disp, GDK_EXPOSURE_MASK |
                                    GDK_ENTER_NOTIFY_MASK |
                                    GDK_LEAVE_NOTIFY_MASK |
                                    GDK_BUTTON_PRESS_MASK);

    g_signal_connect (g->disp, "draw",
                      G_CALLBACK (load_graph_expose), g);
    g_signal_connect (g->disp, "configure_event",
                      G_CALLBACK (load_graph_configure), g);
    g_signal_connect (g->disp, "destroy",
                      G_CALLBACK (load_graph_destroy), g);
    g_signal_connect (g->disp, "button-press-event",
                      G_CALLBACK (load_graph_clicked), g);
    g_signal_connect (g->disp, "enter-notify-event",
                      G_CALLBACK(load_graph_enter_cb), g);
    g_signal_connect (g->disp, "leave-notify-event",
                      G_CALLBACK(load_graph_leave_cb), g);

    gtk_box_pack_start (GTK_BOX (g->box), g->disp, TRUE, TRUE, 0);
    gtk_widget_show_all(g->box);

    return g;
}

void
load_graph_start (LoadGraph *g)
{
    guint event_source_id;

    if (g->timer_index != -1)
        g_source_remove ((guint) g->timer_index);

    event_source_id = g_timeout_add (g->speed,
                                    (GSourceFunc) load_graph_update, g);

    g->timer_index = (gint) event_source_id;
}

void
load_graph_stop (LoadGraph *g)
{
    if (g->timer_index != -1)
        g_source_remove ((guint) g->timer_index);

    g->timer_index = -1;
}
