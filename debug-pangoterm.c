#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ecma48.h"

#include <gtk/gtk.h>

int master;
ecma48_t *e48;

int cell_width;
int cell_height;

GdkRectangle invalid_area;
GdkRectangle cursor_area;

GtkWidget *termwin;
GdkPixmap *termbuffer;

GdkGC *cursor_gc;

typedef struct {
  PangoLayout *layout;
  GdkColor bg_col;
} term_cell;

term_cell **cells;

const char *default_fg = "gray90";
const char *default_bg = "black";

const char *cursor_col = "white";

const char *default_font = "Leonine Sans Mono";
const int default_size = 9;

const char *col_spec[] = {
  "black",
  "red",
  "green",
  "yellow",
  "blue",
  "magenta",
  "cyan",
  "white"
};

typedef struct {
  GdkColor bg_col;
  PangoAttrList *attrs;
} term_pen;

void repaint_area(GdkRectangle *area)
{
  GdkWindow *win = termwin->window;

  GdkGC *gc = gdk_gc_new(win);

  gdk_gc_set_clip_rectangle(gc, area);

  gdk_draw_drawable(win,
      gc,
      termbuffer,
      0, 0, 0, 0, -1, -1);

  g_object_unref(G_OBJECT(gc));
}

void paint_cell(int row, int col, gboolean with_cursor)
{
  GdkGC *gc = gdk_gc_new(termbuffer);

  gdk_gc_set_rgb_fg_color(gc, &cells[row][col].bg_col);

  GdkRectangle destarea = {
    .x      = col * cell_width,
    .y      = row * cell_height,
    .width  = cell_width,
    .height = cell_height
  };

  gdk_draw_rectangle(termbuffer,
      gc,
      TRUE,
      destarea.x,
      destarea.y,
      destarea.width,
      destarea.height);

  PangoLayout *layout = cells[row][col].layout;

  if(layout) {
    gdk_draw_layout(termbuffer,
        gc,
        destarea.x,
        destarea.y,
        layout);
  }

  g_object_unref(G_OBJECT(gc));

  if(with_cursor) {
    gdk_draw_rectangle(termbuffer,
        cursor_gc,
        FALSE,
        destarea.x,
        destarea.y,
        destarea.width - 1,
        destarea.height - 1);
  }

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);
}

gboolean term_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  repaint_area(&event->area);

  if(gdk_rectangle_intersect(&cursor_area, &event->area, NULL))
    gdk_draw_rectangle(termwin->window,
        cursor_gc,
        FALSE,
        cursor_area.x,
        cursor_area.y,
        cursor_area.width - 1,
        cursor_area.height - 1);

  return TRUE;
}

int term_putchar(ecma48_t *e48, uint32_t codepoint, ecma48_position_t pos, void *pen_p)
{
  term_pen *pen = pen_p;

  char s[] = { codepoint, 0 };

  PangoLayout *layout = cells[pos.row][pos.col].layout;

  pango_layout_set_text(layout, s, -1);
  if(pen->attrs)
    pango_layout_set_attributes(layout, pen->attrs);

  cells[pos.row][pos.col].bg_col = pen->bg_col;

  paint_cell(pos.row, pos.col, FALSE);

  return 1;
}

int term_movecursor(ecma48_t *e48, ecma48_position_t pos, ecma48_position_t oldpos)
{
  GdkRectangle destarea = {
    .x      = oldpos.row,
    .y      = oldpos.col,
    .width  = cell_width,
    .height = cell_height
  };

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);

  cursor_area.x      = pos.col * cell_width;
  cursor_area.y      = pos.row * cell_height;
  cursor_area.width  = cell_width;
  cursor_area.height = cell_height;

  return 1;
}

int term_scroll(ecma48_t *e48, ecma48_rectangle_t rect, int downward, int rightward)
{
  GdkGC *gc = gdk_gc_new(termbuffer);

  int rows = rect.end_row - rect.start_row - downward;
  int cols = rect.end_col - rect.start_col - rightward;

  GdkRectangle destarea = {
    .x      = rect.start_col * cell_width,
    .y      = rect.start_row * cell_height,
    .width  = cols * cell_width,
    .height = rows * cell_height
  };

  gdk_draw_drawable(termbuffer,
      gc,
      termbuffer,
      (rect.start_col + rightward) * cell_width,
      (rect.start_row + downward ) * cell_height,
      destarea.x,
      destarea.y,
      destarea.width,
      destarea.height);

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);

  return 0; // Because we still need to get copycell to move the metadata
}

int term_copycell(ecma48_t *e48, ecma48_position_t destpos, ecma48_position_t srcpos)
{
  cells[destpos.row][destpos.col].bg_col = cells[srcpos.row][srcpos.col].bg_col;

  g_object_unref(G_OBJECT(cells[destpos.row][destpos.col].layout));
  cells[destpos.row][destpos.col].layout = 
    pango_layout_copy(cells[srcpos.row][srcpos.col].layout);

  return 1;
}

int term_erase(ecma48_t *e48, ecma48_rectangle_t rect, void *pen_p)
{
  term_pen *pen = pen_p;

  GdkGC *gc = gdk_gc_new(termbuffer);

  gdk_gc_set_rgb_fg_color(gc, &pen->bg_col);

  int row, col;
  for(row = rect.start_row; row < rect.end_row; row++)
    for(col = rect.start_col; col < rect.end_col; col++) {
      cells[row][col].bg_col = pen->bg_col;
      pango_layout_set_text(cells[row][col].layout, "", 0);
    }

  GdkRectangle destarea = {
    .x      = rect.start_col * cell_width,
    .y      = rect.start_row * cell_height,
    .width  = (rect.end_col - rect.start_col) * cell_width,
    .height = (rect.end_row - rect.start_row) * cell_height,
  };

  gdk_draw_rectangle(termbuffer,
      gc,
      TRUE,
      destarea.x,
      destarea.y,
      destarea.width,
      destarea.height);

  g_object_unref(G_OBJECT(gc));

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);

  return 1;
}

int term_setpen(ecma48_t *e48, int sgrcmd, void **penstore)
{
#define CLONEATTRS \
  do { \
    PangoAttrList *newattrs = pango_attr_list_copy(pen->attrs); \
    pango_attr_list_unref(pen->attrs); \
    pen->attrs = newattrs; \
  } while(0)

#define ADDATTR(a) \
  do { \
    PangoAttribute *newattr = (a); \
    newattr->start_index = 0; \
    newattr->end_index = 100; /* can't really know for sure */ \
    pango_attr_list_change(pen->attrs, newattr); \
  } while(0)

  term_pen *pen = *penstore;

  if(!*penstore) {
    pen = g_new0(term_pen, 1);
    *penstore = pen;
    pen->attrs = NULL;
  }

  switch(sgrcmd) {
  case -1:
  case 0: // Reset all
    gdk_color_parse(default_bg, &pen->bg_col);

    if(pen->attrs)
      pango_attr_list_unref(pen->attrs);
    pen->attrs = pango_attr_list_new();

    GdkColor fg_col;
    gdk_color_parse(default_fg, &fg_col);
    ADDATTR(pango_attr_foreground_new(
        fg_col.red * 256, fg_col.green * 256, fg_col.blue * 256)
      );
    break;

  case 1: // Bold
    CLONEATTRS;
    ADDATTR(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    break;

  case 4: // Single underline
    CLONEATTRS;
    ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    break;

  case 21: // Double underline
    CLONEATTRS;
    ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_DOUBLE));
    break;

  case 24: // Not underlined
    CLONEATTRS;
    ADDATTR(pango_attr_underline_new(PANGO_UNDERLINE_NONE));
    break;

  case 30: case 31: case 32: case 33:
  case 34: case 35: case 36: case 37: // Foreground colour
  case 39: // Default foreground
    {
      CLONEATTRS;
      GdkColor fg_col;
      gdk_color_parse(sgrcmd == 39 ? default_fg : col_spec[sgrcmd - 30], &fg_col);
      ADDATTR(pango_attr_foreground_new(
          fg_col.red * 256, fg_col.green * 256, fg_col.blue * 256)
        );
    }
    break;

  case 40: case 41: case 42: case 43:
  case 44: case 45: case 46: case 47: // Background colour
  case 49: // Default background
    gdk_color_parse(sgrcmd == 49 ? default_bg : col_spec[sgrcmd - 40], &pen->bg_col);
    break;

  default:
    return 0;
  }

  return 1;
}

static ecma48_state_callbacks_t cb = {
  .putchar    = term_putchar,
  .movecursor = term_movecursor,
  .scroll     = term_scroll,
  .copycell   = term_copycell,
  .erase      = term_erase,
  .setpen     = term_setpen,
};

gboolean stdin_readable(GIOChannel *source, GIOCondition cond, gpointer data)
{
  char buffer[8192];

  size_t bytes = read(0, buffer, sizeof buffer);

  if(bytes == 0) {
    fprintf(stderr, "STDIN closed\n");
    exit(0);
  }
  if(bytes < 0) {
    fprintf(stderr, "read(STDIN) failed - %s\n", strerror(errno));
    exit(1);
  }

  write(master, buffer, bytes);

  return TRUE;
}

gboolean master_readable(GIOChannel *source, GIOCondition cond, gpointer data)
{
  char buffer[8192];

  size_t bytes = read(master, buffer, sizeof buffer);

  if(bytes == 0) {
    fprintf(stderr, "master closed\n");
    exit(0);
  }
  if(bytes < 0) {
    fprintf(stderr, "read(master) failed - %s\n", strerror(errno));
    exit(1);
  }

  invalid_area.x = 0;
  invalid_area.y = 0;
  invalid_area.width = 0;
  invalid_area.height = 0;

  ecma48_push_bytes(e48, buffer, bytes);

  if(invalid_area.width && invalid_area.height)
    repaint_area(&invalid_area);

  gdk_draw_rectangle(termwin->window,
      cursor_gc,
      FALSE,
      cursor_area.x,
      cursor_area.y,
      cursor_area.width - 1,
      cursor_area.height - 1);

  return TRUE;
}

int main(int argc, char *argv[])
{
  gtk_init(&argc, &argv);

  struct winsize size = { 25, 80, 0, 0 };

  e48 = ecma48_new();
  ecma48_set_size(e48, size.ws_row, size.ws_col);

  ecma48_set_state_callbacks(e48, &cb);

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  termwin = window;

  g_signal_connect(G_OBJECT(window), "expose-event", GTK_SIGNAL_FUNC(term_expose), NULL);

  PangoContext *pctx = gtk_widget_get_pango_context(window);

  PangoFontDescription *fontdesc = pango_font_description_new();
  pango_font_description_set_family(fontdesc, default_font);
  pango_font_description_set_size(fontdesc, default_size * PANGO_SCALE);

  cells = g_new0(term_cell*, size.ws_row);

  int row;
  for(row = 0; row < size.ws_row; row++) {
    cells[row] = g_new0(term_cell, size.ws_col);

    int col;
    for(col = 0; col < size.ws_col; col++) {
      cells[row][col].layout = pango_layout_new(pctx);
      pango_layout_set_font_description(cells[row][col].layout, fontdesc);
    }
  }

  PangoFontMetrics *metrics = pango_context_get_metrics(pctx,
      pango_context_get_font_description(pctx), pango_context_get_language(pctx));

  int width = (pango_font_metrics_get_approximate_char_width(metrics) + 
               pango_font_metrics_get_approximate_digit_width(metrics)) / 2;

  printf("Font metrics: ascent=%d descent=%d width=%d\n", 
      pango_font_metrics_get_ascent(metrics),
      pango_font_metrics_get_descent(metrics),
      width);

  int height = pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics);

  cell_width  = PANGO_PIXELS_CEIL(width);
  cell_height = PANGO_PIXELS_CEIL(height);

  gtk_widget_show_all(window);

  termbuffer = gdk_pixmap_new(window->window,
      size.ws_col * cell_width, size.ws_row * cell_height, -1);

  gtk_window_resize(GTK_WINDOW(window), 
      size.ws_col * cell_width, size.ws_row * cell_height);

  //gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

  cursor_gc = gdk_gc_new(window->window);

  GdkColor col;
  gdk_color_parse(cursor_col, &col);
  gdk_gc_set_rgb_fg_color(cursor_gc, &col);

  ecma48_state_initialise(e48);

  pid_t kid = forkpty(&master, NULL, NULL, &size);
  if(kid == 0) {
    execvp(argv[1], argv + 1);
    fprintf(stderr, "Cannot exec(%s) - %s\n", argv[1], strerror(errno));
    _exit(1);
  }

  GIOChannel *gio_stdin = g_io_channel_unix_new(0);
  g_io_add_watch(gio_stdin, G_IO_IN, stdin_readable, NULL);

  GIOChannel *gio_master = g_io_channel_unix_new(master);
  g_io_add_watch(gio_master, G_IO_IN, master_readable, NULL);

  gtk_main();

  return 0;
}