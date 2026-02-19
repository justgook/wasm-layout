#include <stdint.h>

#define MAX_PANELS 16
#define MAX_HANDLES (MAX_PANELS * 4)
#define MIN_PANEL_SIZE 16

enum layout_error {
  LAYOUT_OK = 0,
  LAYOUT_ERR_NOT_INITIALIZED = 1,
  LAYOUT_ERR_INVALID_ARG = 2,
  LAYOUT_ERR_INVALID_HANDLE = 3,
  LAYOUT_ERR_INVALID_AREA = 4,
  LAYOUT_ERR_INVALID_CORNER = 5,
  LAYOUT_ERR_OUT_OF_BOUNDS = 6,
  LAYOUT_ERR_MIN_SIZE = 7,
};

typedef struct {
  int32_t id;
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} LayoutArea;

typedef struct {
  int32_t id;
  int32_t x;
  int32_t y;
} LayoutHandle;

typedef struct {
  int32_t initialized;
  int32_t screen_w;
  int32_t screen_h;
  int32_t max_panels;
  int32_t area_count;
  int32_t handle_count;
  int32_t last_error;
  int32_t generation;
  int32_t last_action;
  int32_t last_id;
  int32_t last_x;
  int32_t last_y;
  LayoutArea areas[MAX_PANELS];
  LayoutHandle handles[MAX_HANDLES];
} LayoutData;

static LayoutData g_data;

static int32_t find_area_index(int32_t area_id) {
  int32_t i;
  for (i = 0; i < g_data.area_count; ++i) {
    if (g_data.areas[i].id == area_id) {
      return i;
    }
  }
  return -1;
}

static int32_t find_handle_index(int32_t handle_id) {
  int32_t i;
  for (i = 0; i < g_data.handle_count; ++i) {
    if (g_data.handles[i].id == handle_id) {
      return i;
    }
  }
  return -1;
}

static int32_t reject(int32_t err) {
  g_data.last_error = err;
  return err;
}

static void set_handle_positions_from_area(const LayoutArea *area) {
  g_data.handle_count = 4;
  g_data.handles[0].id = 0;
  g_data.handles[0].x = area->x + area->w / 2;
  g_data.handles[0].y = area->y;

  g_data.handles[1].id = 1;
  g_data.handles[1].x = area->x + area->w;
  g_data.handles[1].y = area->y + area->h / 2;

  g_data.handles[2].id = 2;
  g_data.handles[2].x = area->x + area->w / 2;
  g_data.handles[2].y = area->y + area->h;

  g_data.handles[3].id = 3;
  g_data.handles[3].x = area->x;
  g_data.handles[3].y = area->y + area->h / 2;
}

__attribute__((export_name("init_screen"))) int32_t init_screen(int32_t w,
                                                                int32_t h) {
  if (w <= 0 || h <= 0) {
    return reject(LAYOUT_ERR_INVALID_ARG);
  }

  g_data.initialized = 1;
  g_data.screen_w = w;
  g_data.screen_h = h;
  g_data.max_panels = MAX_PANELS;
  g_data.area_count = 1;
  g_data.last_error = LAYOUT_OK;
  g_data.generation += 1;
  g_data.last_action = 1;
  g_data.last_id = 0;
  g_data.last_x = w;
  g_data.last_y = h;

  g_data.areas[0].id = 0;
  g_data.areas[0].x = 0;
  g_data.areas[0].y = 0;
  g_data.areas[0].w = w;
  g_data.areas[0].h = h;

  set_handle_positions_from_area(&g_data.areas[0]);

  return LAYOUT_OK;
}

__attribute__((export_name("move_handle"))) int32_t
move_handle(int32_t handle_id, int32_t x, int32_t y) {
  int32_t idx;
  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }

  idx = find_handle_index(handle_id);
  if (idx < 0) {
    return reject(LAYOUT_ERR_INVALID_HANDLE);
  }

  if (x < 0 || x > g_data.screen_w || y < 0 || y > g_data.screen_h) {
    return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
  }

  g_data.handles[idx].x = x;
  g_data.handles[idx].y = y;
  g_data.last_error = LAYOUT_OK;
  g_data.generation += 1;
  g_data.last_action = 2;
  g_data.last_id = handle_id;
  g_data.last_x = x;
  g_data.last_y = y;

  return LAYOUT_OK;
}

__attribute__((export_name("move_corner"))) int32_t
move_corner(int32_t area_id, int32_t corner_index, int32_t x, int32_t y) {
  int32_t idx;
  int32_t x1;
  int32_t y1;
  int32_t new_x;
  int32_t new_y;
  int32_t new_w;
  int32_t new_h;
  LayoutArea *area;

  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }

  if (corner_index < 0 || corner_index > 3) {
    return reject(LAYOUT_ERR_INVALID_CORNER);
  }

  idx = find_area_index(area_id);
  if (idx < 0) {
    return reject(LAYOUT_ERR_INVALID_AREA);
  }

  if (x < 0 || x > g_data.screen_w || y < 0 || y > g_data.screen_h) {
    return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
  }

  area = &g_data.areas[idx];
  x1 = area->x + area->w;
  y1 = area->y + area->h;
  new_x = area->x;
  new_y = area->y;
  new_w = area->w;
  new_h = area->h;

  if (corner_index == 0) {
    new_x = x;
    new_y = y;
    new_w = x1 - new_x;
    new_h = y1 - new_y;
  } else if (corner_index == 1) {
    new_y = y;
    new_w = x - area->x;
    new_h = y1 - new_y;
  } else if (corner_index == 2) {
    new_w = x - area->x;
    new_h = y - area->y;
  } else {
    new_x = x;
    new_w = x1 - new_x;
    new_h = y - area->y;
  }

  if (new_w < MIN_PANEL_SIZE || new_h < MIN_PANEL_SIZE) {
    return reject(LAYOUT_ERR_MIN_SIZE);
  }

  if (new_x < 0 || new_y < 0 || new_x + new_w > g_data.screen_w ||
      new_y + new_h > g_data.screen_h) {
    return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
  }

  area->x = new_x;
  area->y = new_y;
  area->w = new_w;
  area->h = new_h;
  set_handle_positions_from_area(area);

  g_data.last_error = LAYOUT_OK;
  g_data.generation += 1;
  g_data.last_action = 3;
  g_data.last_id = area_id;
  g_data.last_x = x;
  g_data.last_y = y;

  return LAYOUT_OK;
}

__attribute__((export_name("get_data_ptr"))) int32_t get_data_ptr(void) {
  return (int32_t)(uintptr_t)&g_data;
}
