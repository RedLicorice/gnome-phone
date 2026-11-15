#include "widgets.h"

#include "neo.h"
#include "omni.h"

typedef struct {
  const char *id;
  int         page;
  int         rows;
  void      (*draw) (float x, float y, float w, float h);   /* the widget's box: its rows, the page's full width */
  bool      (*tap)  (float x, float y, float top, float h);   /* true: the tap was on it, and it took it */
} Widget;

/* In order from the top of the page */
static const Widget WIDGETS[] = {
  { "clock",  0, 1, home_clock_draw, NULL },
  { "search", 0, 1, omni_bar_draw,   omni_bar_tap },
};

int
widgets_slots (int page, int cols)
{
  int rows = 0;
  for (guint i = 0; i < G_N_ELEMENTS (WIDGETS); i++)
    if (WIDGETS[i].page == page)
      rows += WIDGETS[i].rows;
  return rows * cols;
}

void
widgets_draw (int page, float x0)
{
  int row = 0;
  for (guint i = 0; i < G_N_ELEMENTS (WIDGETS); i++) {
    if (WIDGETS[i].page != page)
      continue;
    float h, y = home_row_top (row, WIDGETS[i].rows, &h);
    WIDGETS[i].draw (x0, y, N.W, h);
    row += WIDGETS[i].rows;
  }
}

bool
widgets_tap (float x, float y)
{
  int row = 0;
  for (guint i = 0; i < G_N_ELEMENTS (WIDGETS); i++) {
    if (WIDGETS[i].page != 0)
      continue;
    float h, top = home_row_top (row, WIDGETS[i].rows, &h);
    row += WIDGETS[i].rows;
    if (y >= top && y < top + h && WIDGETS[i].tap && WIDGETS[i].tap (x, y, top, h))
      return true;
  }
  return false;
}
