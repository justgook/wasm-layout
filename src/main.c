#include "./layout.h"
#include <stdint.h>

#define MAX_PANELS 16
#define MIN_PANEL_SIZE 16
#define HANDLE_HALF_SIZE 4

#define MAX_HANDLES (MAX_PANELS - 1)
#define MAX_SEGMENTS (MAX_PANELS * MAX_PANELS)

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

static int32_t overlap_len(int32_t a0, int32_t a1, int32_t b0, int32_t b1) {
  int32_t lo = a0 > b0 ? a0 : b0;
  int32_t hi = a1 < b1 ? a1 : b1;
  return hi - lo;
}

/*
 * BoundarySegment: a contiguous divider line at a given coordinate.
 *   axis=0 (vertical):   coord=x position, a0..a1=y range
 *   axis=1 (horizontal): coord=y position, a0..a1=x range
 */
typedef struct {
  int32_t coord;
  int32_t a0;
  int32_t a1;
} BoundarySegment;

static void sort_segments(BoundarySegment *segs, int32_t count) {
  int32_t i;
  int32_t j;
  for (i = 0; i < count; ++i) {
    for (j = i + 1; j < count; ++j) {
      int32_t swap = 0;
      if (segs[j].coord < segs[i].coord) {
        swap = 1;
      } else if (segs[j].coord == segs[i].coord && segs[j].a0 < segs[i].a0) {
        swap = 1;
      }
      if (swap) {
        BoundarySegment t = segs[i];
        segs[i] = segs[j];
        segs[j] = t;
      }
    }
  }
}

/*
 * Collect all boundary segments for a given axis.
 * Always merge touching or overlapping segments at the same coordinate.
 * This produces full-length divider lines even when multiple area pairs
 * contribute to the same logical divider (e.g. T-junctions, 3-col layouts).
 */
static int32_t collect_segments(int32_t axis, BoundarySegment *out,
                                int32_t max_out) {
  int32_t i;
  int32_t j;
  int32_t raw_count = 0;
  BoundarySegment raw[MAX_SEGMENTS];
  int32_t out_count;

  for (i = 0; i < g_data.area_count; ++i) {
    for (j = 0; j < g_data.area_count; ++j) {
      int32_t coord;
      int32_t s0;
      int32_t s1;
      LayoutArea *a;
      LayoutArea *b;
      if (i == j) {
        continue;
      }
      a = &g_data.areas[i];
      b = &g_data.areas[j];

      if (axis == 0) {
        if (a->x1 != b->x0) {
          continue;
        }
        coord = a->x1;
        s0 = a->y0 > b->y0 ? a->y0 : b->y0;
        s1 = a->y1 < b->y1 ? a->y1 : b->y1;
      } else {
        if (a->y1 != b->y0) {
          continue;
        }
        coord = a->y1;
        s0 = a->x0 > b->x0 ? a->x0 : b->x0;
        s1 = a->x1 < b->x1 ? a->x1 : b->x1;
      }

      if (s1 <= s0) {
        continue;
      }
      if (raw_count >= MAX_SEGMENTS) {
        break;
      }
      raw[raw_count].coord = coord;
      raw[raw_count].a0 = s0;
      raw[raw_count].a1 = s1;
      raw_count += 1;
    }
  }

  if (raw_count == 0) {
    return 0;
  }

  sort_segments(raw, raw_count);

  out_count = 0;
  for (i = 0; i < raw_count; ++i) {
    if (out_count == 0) {
      if (out_count < max_out) {
        out[out_count++] = raw[i];
      }
      continue;
    }

    if (out[out_count - 1].coord == raw[i].coord &&
        raw[i].a0 <= out[out_count - 1].a1) {
      /* touching or overlapping at same coord -> merge */
      if (raw[i].a1 > out[out_count - 1].a1) {
        out[out_count - 1].a1 = raw[i].a1;
      }
    } else if (out_count < max_out) {
      out[out_count++] = raw[i];
    }
  }

  return out_count;
}

/*
 * Find the contiguous boundary sub-range at a given coord that covers
 * the handle midpoint. This prevents merging independent handles that
 * happen to touch at one point (e.g. two horizontal handles meeting
 * at a vertical split).
 *
 * Walk the raw (unmerged) boundary pairs and flood-fill from the handle's
 * midpoint to find only the connected portion.
 */
static void find_contiguous_boundary(int32_t axis, int32_t coord, int32_t h_mid,
                                     int32_t h_span0, int32_t h_span1,
                                     int32_t *out_a0, int32_t *out_a1) {
  int32_t i;
  int32_t j;
  int32_t raw_count = 0;
  int32_t a0s[MAX_SEGMENTS];
  int32_t a1s[MAX_SEGMENTS];
  int32_t merged;
  int32_t region_a0;
  int32_t region_a1;

  /* collect raw boundary sub-segments at this coord */
  for (i = 0; i < g_data.area_count; ++i) {
    for (j = 0; j < g_data.area_count; ++j) {
      int32_t s0;
      int32_t s1;
      LayoutArea *a;
      LayoutArea *b;
      if (i == j) {
        continue;
      }
      a = &g_data.areas[i];
      b = &g_data.areas[j];

      if (axis == 0) {
        if (a->x1 != coord || b->x0 != coord) {
          continue;
        }
        s0 = a->y0 > b->y0 ? a->y0 : b->y0;
        s1 = a->y1 < b->y1 ? a->y1 : b->y1;
      } else {
        if (a->y1 != coord || b->y0 != coord) {
          continue;
        }
        s0 = a->x0 > b->x0 ? a->x0 : b->x0;
        s1 = a->x1 < b->x1 ? a->x1 : b->x1;
      }

      if (s1 <= s0 || raw_count >= MAX_SEGMENTS) {
        continue;
      }
      a0s[raw_count] = s0;
      a1s[raw_count] = s1;
      raw_count += 1;
    }
  }

  if (raw_count == 0) {
    *out_a0 = 0;
    *out_a1 = 0;
    return;
  }

  /* find the sub-segment that contains h_mid, then flood-fill connected */
  region_a0 = 0;
  region_a1 = 0;
  for (i = 0; i < raw_count; ++i) {
    if (h_mid >= a0s[i] && h_mid <= a1s[i]) {
      region_a0 = a0s[i];
      region_a1 = a1s[i];
      break;
    }
  }

  if (region_a0 == region_a1) {
    /* midpoint not inside any sub-segment, pick closest */
    int32_t best_d = 0x7fffffff;
    for (i = 0; i < raw_count; ++i) {
      int32_t mid = (a0s[i] + a1s[i]) / 2;
      int32_t d = mid - h_mid;
      if (d < 0) {
        d = -d;
      }
      if (d < best_d) {
        best_d = d;
        region_a0 = a0s[i];
        region_a1 = a1s[i];
      }
    }
  }

  /*
   * Expand region by merging overlapping sub-segments.
   * For sub-segments that merely touch (share one endpoint), only merge
   * if the handle's current span overlaps BOTH sub-segments.
   *
   * This correctly handles + junctions: a vertical handle spanning the
   * full height will merge both halves of the vertical boundary, while
   * horizontal handles confined to one column will not expand to the other.
   */
  do {
    merged = 0;
    for (i = 0; i < raw_count; ++i) {
      int32_t do_merge = 0;
      if (a0s[i] < region_a1 && a1s[i] > region_a0) {
        /* strict overlap -> always merge */
        do_merge = 1;
      } else if (a0s[i] == region_a1 || a1s[i] == region_a0) {
        /*
         * Touching at one point. Merge only if the handle's current
         * span overlaps the candidate sub-segment. This ensures that
         * a handle only "claims" sub-segments it already covers,
         * preventing handles from jumping across perpendicular splits.
         */
        if (overlap_len(a0s[i], a1s[i], h_span0, h_span1) > 0) {
          do_merge = 1;
        }
      }
      if (do_merge) {
        if (a0s[i] < region_a0) {
          region_a0 = a0s[i];
          merged = 1;
        }
        if (a1s[i] > region_a1) {
          region_a1 = a1s[i];
          merged = 1;
        }
      }
    }
  } while (merged);

  *out_a0 = region_a0;
  *out_a1 = region_a1;
}

/*
 * Match a handle to its corresponding boundary segment.
 * Uses contiguous boundary flood-fill from handle midpoint to determine
 * exact span. This keeps handles independent even when their segments
 * touch at a single point (e.g. + layout, or two horizontal handles
 * meeting at a vertical divider).
 */
static int32_t update_handle_from_topology(int32_t handle_index) {
  int32_t axis;
  int32_t target_coord;
  int32_t h_span0;
  int32_t h_span1;
  int32_t h_mid;
  int32_t best = -1;
  int32_t best_score = 0x7fffffff;
  int32_t i;
  BoundarySegment segs[MAX_SEGMENTS];
  int32_t seg_count;
  LayoutHandle *h;
  int32_t region_a0;
  int32_t region_a1;

  if (!is_valid_handle_index(handle_index)) {
    return 0;
  }

  h = &g_data.handles[handle_index];
  axis = (h->x1 - h->x0) <= (h->y1 - h->y0) ? 0 : 1;

  if (axis == 0) {
    target_coord = (h->x0 + h->x1) / 2;
    h_span0 = h->y0;
    h_span1 = h->y1;
    h_mid = (h_span0 + h_span1) / 2;
  } else {
    target_coord = (h->y0 + h->y1) / 2;
    h_span0 = h->x0;
    h_span1 = h->x1;
    h_mid = (h_span0 + h_span1) / 2;
  }

  seg_count = collect_segments(axis, segs, MAX_SEGMENTS);
  if (seg_count <= 0) {
    return 0;
  }

  /* find best matching merged segment (for coord) */
  for (i = 0; i < seg_count; ++i) {
    int32_t d_coord = segs[i].coord - target_coord;
    int32_t seg_contains_mid;
    int32_t spans_overlap;
    int32_t score;

    if (d_coord < 0) {
      d_coord = -d_coord;
    }

    seg_contains_mid = (h_mid >= segs[i].a0 && h_mid <= segs[i].a1);
    spans_overlap = overlap_len(segs[i].a0, segs[i].a1, h_span0, h_span1) > 0;

    if (d_coord == 0 && seg_contains_mid) {
      score = 0;
    } else if (d_coord == 0 && spans_overlap) {
      score = 1;
    } else {
      int32_t d_mid = ((segs[i].a0 + segs[i].a1) / 2) - h_mid;
      if (d_mid < 0) {
        d_mid = -d_mid;
      }
      score = 2 + d_coord * 4096 + d_mid;
    }

    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }

  if (best < 0) {
    return 0;
  }

  /*
   * Instead of using the full merged segment span, find the contiguous
   * sub-range that actually covers the handle's midpoint. This prevents
   * two handles from "sticking" when they touch at a single point.
   */
  find_contiguous_boundary(axis, segs[best].coord, h_mid, h_span0, h_span1,
                           &region_a0, &region_a1);

  if (region_a0 >= region_a1) {
    /* fallback: use full segment */
    region_a0 = segs[best].a0;
    region_a1 = segs[best].a1;
  }

  if (axis == 0) {
    int32_t x = segs[best].coord;
    h->x0 = clamp_i32(x - HANDLE_HALF_SIZE, 0, g_data.screen_w);
    h->x1 = clamp_i32(x + HANDLE_HALF_SIZE, 0, g_data.screen_w);
    h->y0 = clamp_i32(region_a0, 0, g_data.screen_h);
    h->y1 = clamp_i32(region_a1, 0, g_data.screen_h);
  } else {
    int32_t y = segs[best].coord;
    h->x0 = clamp_i32(region_a0, 0, g_data.screen_w);
    h->x1 = clamp_i32(region_a1, 0, g_data.screen_w);
    h->y0 = clamp_i32(y - HANDLE_HALF_SIZE, 0, g_data.screen_h);
    h->y1 = clamp_i32(y + HANDLE_HALF_SIZE, 0, g_data.screen_h);
  }

  return 1;
}

static void refresh_all_handles_from_topology(void) {
  int32_t i;
  for (i = 0; i < g_data.handle_count; ++i) {
    update_handle_from_topology(i);
  }
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
                                const LayoutArea *before_split,
                                int32_t area_a_index, int32_t area_b_index) {
  LayoutHandle *h;
  int32_t idx;
  (void)area_a_index;
  (void)area_b_index;
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

int32_t init_screen(int32_t w, int32_t h) {
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

int32_t resize_screen(int32_t w, int32_t h) {
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

  refresh_all_handles_from_topology();

  set_ok(6, 0, w, h);
  return LAYOUT_OK;
}

int32_t move_handle(int32_t handle_index, int32_t x, int32_t y) {
  LayoutHandle *hnd;
  int32_t axis;
  int32_t split;
  int32_t span0;
  int32_t span1;
  int32_t left_idxs[MAX_PANELS];
  int32_t right_idxs[MAX_PANELS];
  int32_t left_count = 0;
  int32_t right_count = 0;
  int32_t i;

  if (!g_data.initialized) {
    return reject(LAYOUT_ERR_NOT_INITIALIZED);
  }
  if (!is_valid_handle_index(handle_index)) {
    return reject(LAYOUT_ERR_INVALID_HANDLE);
  }
  if (x < 0 || x > g_data.screen_w || y < 0 || y > g_data.screen_h) {
    return reject(LAYOUT_ERR_OUT_OF_BOUNDS);
  }

  refresh_all_handles_from_topology();

  hnd = &g_data.handles[handle_index];
  axis = (hnd->x1 - hnd->x0) <= (hnd->y1 - hnd->y0) ? 0 : 1;

  if (axis == 0) {
    split = (hnd->x0 + hnd->x1) / 2;
    span0 = hnd->y0;
    span1 = hnd->y1;
    for (i = 0; i < g_data.area_count; ++i) {
      LayoutArea *a = &g_data.areas[i];
      if (a->x1 == split && overlap_len(a->y0, a->y1, span0, span1) > 0) {
        left_idxs[left_count++] = i;
      }
      if (a->x0 == split && overlap_len(a->y0, a->y1, span0, span1) > 0) {
        right_idxs[right_count++] = i;
      }
    }
    if (left_count == 0 || right_count == 0) {
      return reject(LAYOUT_ERR_NOT_IMPLEMENTED);
    }

    split = x;
    for (i = 0; i < left_count; ++i) {
      LayoutArea *a = &g_data.areas[left_idxs[i]];
      if (split <= a->x0 + MIN_PANEL_SIZE) {
        return reject(LAYOUT_ERR_MIN_SIZE);
      }
    }
    for (i = 0; i < right_count; ++i) {
      LayoutArea *a = &g_data.areas[right_idxs[i]];
      if (split >= a->x1 - MIN_PANEL_SIZE) {
        return reject(LAYOUT_ERR_MIN_SIZE);
      }
    }

    for (i = 0; i < g_data.area_count; ++i) {
      int32_t touched = 0;
      int32_t j;
      for (j = 0; j < left_count; ++j) {
        if (left_idxs[j] == i) {
          touched = 1;
          break;
        }
      }
      for (j = 0; j < right_count && !touched; ++j) {
        if (right_idxs[j] == i) {
          touched = 1;
        }
      }
      if (!touched) {
        LayoutArea *a = &g_data.areas[i];
        if (overlap_len(a->y0, a->y1, span0, span1) > 0 && a->x0 < split &&
            split < a->x1) {
          return reject(LAYOUT_ERR_NOT_IMPLEMENTED);
        }
      }
    }

    for (i = 0; i < left_count; ++i) {
      g_data.areas[left_idxs[i]].x1 = split;
    }
    for (i = 0; i < right_count; ++i) {
      g_data.areas[right_idxs[i]].x0 = split;
    }
    hnd->x0 = clamp_i32(split - HANDLE_HALF_SIZE, 0, g_data.screen_w);
    hnd->x1 = clamp_i32(split + HANDLE_HALF_SIZE, 0, g_data.screen_w);
  } else {
    split = (hnd->y0 + hnd->y1) / 2;
    span0 = hnd->x0;
    span1 = hnd->x1;
    for (i = 0; i < g_data.area_count; ++i) {
      LayoutArea *a = &g_data.areas[i];
      if (a->y1 == split && overlap_len(a->x0, a->x1, span0, span1) > 0) {
        left_idxs[left_count++] = i;
      }
      if (a->y0 == split && overlap_len(a->x0, a->x1, span0, span1) > 0) {
        right_idxs[right_count++] = i;
      }
    }
    if (left_count == 0 || right_count == 0) {
      return reject(LAYOUT_ERR_NOT_IMPLEMENTED);
    }

    split = y;
    for (i = 0; i < left_count; ++i) {
      LayoutArea *a = &g_data.areas[left_idxs[i]];
      if (split <= a->y0 + MIN_PANEL_SIZE) {
        return reject(LAYOUT_ERR_MIN_SIZE);
      }
    }
    for (i = 0; i < right_count; ++i) {
      LayoutArea *a = &g_data.areas[right_idxs[i]];
      if (split >= a->y1 - MIN_PANEL_SIZE) {
        return reject(LAYOUT_ERR_MIN_SIZE);
      }
    }

    for (i = 0; i < g_data.area_count; ++i) {
      int32_t touched = 0;
      int32_t j;
      for (j = 0; j < left_count; ++j) {
        if (left_idxs[j] == i) {
          touched = 1;
          break;
        }
      }
      for (j = 0; j < right_count && !touched; ++j) {
        if (right_idxs[j] == i) {
          touched = 1;
        }
      }
      if (!touched) {
        LayoutArea *a = &g_data.areas[i];
        if (overlap_len(a->x0, a->x1, span0, span1) > 0 && a->y0 < split &&
            split < a->y1) {
          return reject(LAYOUT_ERR_NOT_IMPLEMENTED);
        }
      }
    }

    for (i = 0; i < left_count; ++i) {
      g_data.areas[left_idxs[i]].y1 = split;
    }
    for (i = 0; i < right_count; ++i) {
      g_data.areas[right_idxs[i]].y0 = split;
    }
    hnd->y0 = clamp_i32(split - HANDLE_HALF_SIZE, 0, g_data.screen_h);
    hnd->y1 = clamp_i32(split + HANDLE_HALF_SIZE, 0, g_data.screen_h);
  }

  refresh_all_handles_from_topology();
  set_ok(2, handle_index, x, y);
  return LAYOUT_OK;
}

int32_t move_corner(int32_t area_index, int32_t corner_index, int32_t x,
                    int32_t y) {
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
    if (g_data.area_count >= MAX_PANELS || g_data.handle_count >= MAX_HANDLES) {
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
    err = add_split_handle(is_vertical, split, &source, area_index,
                           g_data.area_count - 1);
    if (err != LAYOUT_OK) {
      return reject(err);
    }

    refresh_all_handles_from_topology();
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

int32_t set_area_content(int32_t area_index, int32_t content_id) {
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

int32_t set_handle_content(int32_t handle_index, int32_t content_id) {
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

int32_t get_data_ptr(void) { return (int32_t)(uintptr_t)&g_data; }
