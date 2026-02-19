#include <stdint.h>

#define MAX_PANELS 16
#define MAX_HANDLES (MAX_PANELS * 4)
#define MIN_PANEL_SIZE 16
#define HANDLE_HALF_SIZE 4

enum layout_error {
  LAYOUT_OK = 0,
  LAYOUT_ERR_NOT_INITIALIZED = 1,
  LAYOUT_ERR_INVALID_ARG = 2,
  LAYOUT_ERR_INVALID_HANDLE = 3,
  LAYOUT_ERR_INVALID_AREA = 4,
  LAYOUT_ERR_INVALID_CORNER = 5,
  LAYOUT_ERR_OUT_OF_BOUNDS = 6,
  LAYOUT_ERR_MIN_SIZE = 7,
  LAYOUT_ERR_NOT_IMPLEMENTED = 8,
  LAYOUT_ERR_CAPACITY = 9,
};

typedef struct {
  int32_t x0;
  int32_t y0;
  int32_t x1;
  int32_t y1;
  int32_t content_id;
} LayoutArea;

typedef struct {
  int32_t x0;
  int32_t y0;
  int32_t x1;
  int32_t y1;
  int32_t content_id;
} LayoutHandle;

typedef struct {
  int32_t initialized;
  int32_t screen_w;
  int32_t screen_h;
  int32_t max_panels;
  int32_t max_handles;
  int32_t area_count;
  int32_t handle_count;
  int32_t last_error;
  int32_t generation;
  int32_t last_action;
  int32_t last_index;
  int32_t last_x;
  int32_t last_y;
  LayoutArea areas[MAX_PANELS];
  LayoutHandle handles[MAX_HANDLES];
} LayoutData;

static LayoutData g_data;

static int32_t is_valid_area_index(int32_t idx) {
  return idx >= 0 && idx < g_data.area_count;
}

static int32_t is_valid_handle_index(int32_t idx) {
  return idx >= 0 && idx < g_data.handle_count;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static int32_t scale_coord(int32_t value, int32_t old_size, int32_t new_size) {
  if (old_size <= 0) {
    return 0;
  }
  return (int32_t)(((int64_t)value * (int64_t)new_size) / (int64_t)old_size);
}

static int32_t reject(int32_t err) {
  g_data.last_error = err;
  return err;
}

static void set_ok(int32_t action, int32_t index, int32_t x, int32_t y) {
  g_data.last_error = LAYOUT_OK;
  g_data.generation += 1;
  g_data.last_action = action;
  g_data.last_index = index;
  g_data.last_x = x;
  g_data.last_y = y;
}

static int32_t point_in_area_strict(const LayoutArea *a, int32_t x, int32_t y) {
  return x > a->x0 && x < a->x1 && y > a->y0 && y < a->y1;
}

static int32_t point_in_area_closed_open(const LayoutArea *a, int32_t x,
                                         int32_t y) {
  return x >= a->x0 && x < a->x1 && y >= a->y0 && y < a->y1;
}

static void set_handle_center(int32_t handle_index, int32_t cx, int32_t cy) {
  LayoutHandle *h;
  if (!is_valid_handle_index(handle_index)) {
    return;
  }

  h = &g_data.handles[handle_index];
  h->x0 = clamp_i32(cx - HANDLE_HALF_SIZE, 0, g_data.screen_w);
  h->y0 = clamp_i32(cy - HANDLE_HALF_SIZE, 0, g_data.screen_h);
  h->x1 = clamp_i32(cx + HANDLE_HALF_SIZE, 0, g_data.screen_w);
  h->y1 = clamp_i32(cy + HANDLE_HALF_SIZE, 0, g_data.screen_h);
}

static int32_t add_split_handle(int32_t is_vertical, int32_t split_coord,
                                const LayoutArea *before_split) {
  LayoutHandle *h;
  int32_t idx;
  if (g_data.handle_count >= MAX_HANDLES) {
    return LAYOUT_ERR_CAPACITY;
  }

  idx = g_data.handle_count;
  g_data.handle_count += 1;
  h = &g_data.handles[idx];
  h->content_id = 0;

  if (is_vertical) {
    h->x0 = clamp_i32(split_coord - HANDLE_HALF_SIZE, 0, g_data.screen_w);
    h->x1 = clamp_i32(split_coord + HANDLE_HALF_SIZE, 0, g_data.screen_w);
    h->y0 = before_split->y0;
    h->y1 = before_split->y1;
  } else {
    h->x0 = before_split->x0;
    h->x1 = before_split->x1;
    h->y0 = clamp_i32(split_coord - HANDLE_HALF_SIZE, 0, g_data.screen_h);
    h->y1 = clamp_i32(split_coord + HANDLE_HALF_SIZE, 0, g_data.screen_h);
  }

  return LAYOUT_OK;
}

static int32_t prefers_vertical_split(const LayoutArea *a, int32_t corner,
                                      int32_t x, int32_t y) {
  int32_t dx = 0;
  int32_t dy = 0;
  if (corner == 0) {
    dx = x - a->x0;
    dy = y - a->y0;
  } else if (corner == 1) {
    dx = a->x1 - x;
    dy = y - a->y0;
  } else if (corner == 2) {
    dx = a->x1 - x;
    dy = a->y1 - y;
  } else {
    dx = x - a->x0;
    dy = a->y1 - y;
  }
  return dx >= dy;
}

__attribute__((export_name("init_screen"))) int32_t init_screen(int32_t w,
                                                                int32_t h) {
  int32_t i;
  if (w <= 0 || h <= 0) {
    return reject(LAYOUT_ERR_INVALID_ARG);
  }

  g_data.initialized = 1;
  g_data.screen_w = w;
  g_data.screen_h = h;
  g_data.max_panels = MAX_PANELS;
  g_data.max_handles = MAX_HANDLES;
  g_data.area_count = 1;
  g_data.handle_count = 0;

  g_data.areas[0].x0 = 0;
  g_data.areas[0].y0 = 0;
  g_data.areas[0].x1 = w;
  g_data.areas[0].y1 = h;
  g_data.areas[0].content_id = 0;

  for (i = 1; i < MAX_PANELS; ++i) {
    g_data.areas[i].x0 = 0;
    g_data.areas[i].y0 = 0;
    g_data.areas[i].x1 = 0;
    g_data.areas[i].y1 = 0;
    g_data.areas[i].content_id = 0;
  }

  for (i = 0; i < MAX_HANDLES; ++i) {
    g_data.handles[i].x0 = 0;
    g_data.handles[i].y0 = 0;
    g_data.handles[i].x1 = 0;
    g_data.handles[i].y1 = 0;
    g_data.handles[i].content_id = 0;
  }

  set_ok(1, 0, w, h);
  return LAYOUT_OK;
}

__attribute__((export_name("resize_screen"))) int32_t resize_screen(int32_t w,
                                                                     int32_t h) {
  int32_t i;
  int32_t old_w;
  int32_t old_h;
  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }
  if (w <= 0 || h <= 0) {
    return reject(LAYOUT_ERR_INVALID_ARG);
  }

  old_w = g_data.screen_w;
  old_h = g_data.screen_h;
  g_data.screen_w = w;
  g_data.screen_h = h;

  for (i = 0; i < g_data.area_count; ++i) {
    LayoutArea *a = &g_data.areas[i];
    a->x0 = scale_coord(a->x0, old_w, w);
    a->y0 = scale_coord(a->y0, old_h, h);
    a->x1 = scale_coord(a->x1, old_w, w);
    a->y1 = scale_coord(a->y1, old_h, h);
    a->x0 = clamp_i32(a->x0, 0, w);
    a->y0 = clamp_i32(a->y0, 0, h);
    a->x1 = clamp_i32(a->x1, 0, w);
    a->y1 = clamp_i32(a->y1, 0, h);
    if (a->x1 <= a->x0) {
      a->x1 = clamp_i32(a->x0 + 1, 1, w);
    }
    if (a->y1 <= a->y0) {
      a->y1 = clamp_i32(a->y0 + 1, 1, h);
    }
  }

  for (i = 0; i < g_data.handle_count; ++i) {
    LayoutHandle *hnd = &g_data.handles[i];
    hnd->x0 = scale_coord(hnd->x0, old_w, w);
    hnd->y0 = scale_coord(hnd->y0, old_h, h);
    hnd->x1 = scale_coord(hnd->x1, old_w, w);
    hnd->y1 = scale_coord(hnd->y1, old_h, h);
    hnd->x0 = clamp_i32(hnd->x0, 0, w);
    hnd->y0 = clamp_i32(hnd->y0, 0, h);
    hnd->x1 = clamp_i32(hnd->x1, 0, w);
    hnd->y1 = clamp_i32(hnd->y1, 0, h);
    if (hnd->x1 <= hnd->x0) {
      hnd->x1 = clamp_i32(hnd->x0 + 1, 1, w);
    }
    if (hnd->y1 <= hnd->y0) {
      hnd->y1 = clamp_i32(hnd->y0 + 1, 1, h);
    }
  }

  set_ok(6, 0, w, h);
  return LAYOUT_OK;
}

__attribute__((export_name("move_handle"))) int32_t
move_handle(int32_t handle_index, int32_t x, int32_t y) {
  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }
  if (!is_valid_handle_index(handle_index)) {
    return reject(LAYOUT_ERR_INVALID_HANDLE);
  }
  if (x < 0 || x > g_data.screen_w || y < 0 || y > g_data.screen_h) {
    return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
  }

  set_handle_center(handle_index, x, y);
  set_ok(2, handle_index, x, y);
  return LAYOUT_OK;
}

__attribute__((export_name("move_corner"))) int32_t
move_corner(int32_t area_index, int32_t corner_index, int32_t x, int32_t y) {
  LayoutArea source;
  LayoutArea *dst_new;
  LayoutArea *dst_old;
  int32_t is_vertical;
  int32_t split;
  int32_t i;
  int32_t err;

  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }
  if (corner_index < 0 || corner_index > 3) {
    return reject(LAYOUT_ERR_INVALID_CORNER);
  }
  if (!is_valid_area_index(area_index)) {
    return reject(LAYOUT_ERR_INVALID_AREA);
  }
  if (x < 0 || x > g_data.screen_w || y < 0 || y > g_data.screen_h) {
    return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
  }

  source = g_data.areas[area_index];

  if (point_in_area_strict(&source, x, y)) {
    if (g_data.area_count >= MAX_PANELS) {
      return reject(LAYOUT_ERR_CAPACITY);
    }

    is_vertical = prefers_vertical_split(&source, corner_index, x, y);
    if (is_vertical) {
      split = x;
      if (split <= source.x0 + MIN_PANEL_SIZE ||
          split >= source.x1 - MIN_PANEL_SIZE) {
        return reject(LAYOUT_ERR_MIN_SIZE);
      }

      dst_old = &g_data.areas[area_index];
      dst_new = &g_data.areas[g_data.area_count];
      dst_old->x0 = source.x0;
      dst_old->y0 = source.y0;
      dst_old->x1 = split;
      dst_old->y1 = source.y1;
      dst_new->x0 = split;
      dst_new->y0 = source.y0;
      dst_new->x1 = source.x1;
      dst_new->y1 = source.y1;
      dst_new->content_id = source.content_id;
    } else {
      split = y;
      if (split <= source.y0 + MIN_PANEL_SIZE ||
          split >= source.y1 - MIN_PANEL_SIZE) {
        return reject(LAYOUT_ERR_MIN_SIZE);
      }

      dst_old = &g_data.areas[area_index];
      dst_new = &g_data.areas[g_data.area_count];
      dst_old->x0 = source.x0;
      dst_old->y0 = source.y0;
      dst_old->x1 = source.x1;
      dst_old->y1 = split;
      dst_new->x0 = source.x0;
      dst_new->y0 = split;
      dst_new->x1 = source.x1;
      dst_new->y1 = source.y1;
      dst_new->content_id = source.content_id;
    }

    g_data.area_count += 1;
    err = add_split_handle(is_vertical, split, &source);
    if (err != LAYOUT_OK) {
      return reject(err);
    }

    set_ok(3, area_index, x, y);
    return LAYOUT_OK;
  }

  for (i = 0; i < g_data.area_count; ++i) {
    if (i == area_index) {
      continue;
    }
    if (point_in_area_closed_open(&g_data.areas[i], x, y)) {
      return reject(LAYOUT_ERR_NOT_IMPLEMENTED);
    }
  }

  return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
}

__attribute__((export_name("set_area_content"))) int32_t
set_area_content(int32_t area_index, int32_t content_id) {
  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }
  if (!is_valid_area_index(area_index)) {
    return reject(LAYOUT_ERR_INVALID_AREA);
  }

  g_data.areas[area_index].content_id = content_id;
  set_ok(4, area_index, content_id, 0);
  return LAYOUT_OK;
}

__attribute__((export_name("set_handle_content"))) int32_t
set_handle_content(int32_t handle_index, int32_t content_id) {
  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }
  if (!is_valid_handle_index(handle_index)) {
    return reject(LAYOUT_ERR_INVALID_HANDLE);
  }

  g_data.handles[handle_index].content_id = content_id;
  set_ok(5, handle_index, content_id, 0);
  return LAYOUT_OK;
}

__attribute__((export_name("get_data_ptr"))) int32_t get_data_ptr(void) {
  return (int32_t)(uintptr_t)&g_data;
}
