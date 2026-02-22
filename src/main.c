/*
 * layout/src/main.c
 *
 * Headless rectangular layout manager — Blender-inspired.
 * See spec/README.md for the full specification.
 *
 * Implementation strategy (v0 — functional mock):
 *   - Flat arrays for areas and handles stored inside LayoutInfo.
 *   - Handles carry explicit span (x0,y0,x1,y1) and axis.
 *   - Split: determine axis from drag delta, create new area + handle.
 *   - Merge: NOT_IMPLEMENTED (returns error, no state change).
 *   - Handle move: update boundary coordinate for all areas on both sides.
 *   - Resize: proportional scale of all coordinates (integer truncation).
 *
 * See spec/ADR-001-data-model.md for rationale.
 */

#include "layout.h"

/*
 * LAYOUT_EXPORT is #undef'd at the end of layout.h.
 * Re-define it here so function definitions in this TU also carry the
 * wasm export attribute (needed by zig's -rdynamic / --export-dynamic).
 */
#if defined(__wasm__)
#define LAYOUT_EXPORT(name) __attribute__((export_name(name)))
#else
#define LAYOUT_EXPORT(name)
#endif

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Absolute value for layout_i32 */
static layout_i32 abs_i32(layout_i32 v) { return v < 0 ? -v : v; }

/* Integer truncating multiply-divide: (v * num) / den */
static layout_i32 scale_i32(layout_i32 v, layout_i32 num, layout_i32 den) {
    /* Use wider arithmetic to avoid overflow on large coordinates.      */
    /* C99 guarantees signed integer division truncates toward zero.     */
    return (layout_i32)(((long long)v * num) / den);
}

/* ── Handle axis tag ───────────────────────────────────────────────── */
/*
 * We need to know whether a handle is vertical (moves left/right) or
 * horizontal (moves up/down).  We store this in a parallel array rather
 * than inside LayoutHandle so the public struct stays ABI-stable.
 */
#define AXIS_VERTICAL   0   /* boundary is a vertical line (x = const) */
#define AXIS_HORIZONTAL 1   /* boundary is a horizontal line (y = const) */

/* ── Static state ──────────────────────────────────────────────────── */

static LayoutInfo g_info;

/*
 * Per-handle metadata — indexed in parallel with g_info.handles[].
 * Not exposed in the snapshot.
 *
 * g_handle_axis:   AXIS_VERTICAL or AXIS_HORIZONTAL.
 *
 * g_handle_col_x0 / g_handle_col_x1:
 *   For HORIZONTAL handles only.  The left and right x-coordinates of
 *   the vertical boundaries that bound this handle's column.  These are
 *   the "walls" that define which areas belong to this handle.
 *
 *   Stored explicitly because the handle's rendered x0/x1 rect fields
 *   cannot serve as column bounds after a vertical boundary moves — the
 *   rect is updated to the new span, but we still need to know which
 *   vertical walls define the column so we can filter correctly.
 *
 *   Updated whenever a vertical boundary moves (move_handle on a
 *   AXIS_VERTICAL handle).
 *
 * g_handle_row_y0 / g_handle_row_y1:
 *   For VERTICAL handles only.  The top and bottom y-coordinates of
 *   the horizontal boundaries that bound this handle's row.
 *
 *   Updated whenever a horizontal boundary moves (move_handle on a
 *   AXIS_HORIZONTAL handle).
 */
static layout_i32 g_handle_axis[MAX_HANDLES];
static layout_i32 g_handle_col_x0[MAX_HANDLES];
static layout_i32 g_handle_col_x1[MAX_HANDLES];
static layout_i32 g_handle_row_y0[MAX_HANDLES];
static layout_i32 g_handle_row_y1[MAX_HANDLES];

/* ── Forward declarations ──────────────────────────────────────────── */

static void rebuild_handles(void);
static void recompute_handle_rect_scoped(layout_i32 idx);

/* ── Snapshot helpers ──────────────────────────────────────────────── */

static void set_error(layout_i32 code) {
    g_info.last_error = code;
}

static void bump_generation(void) {
    g_info.generation += 1;
}

static void set_try_preview(layout_i32 valid, layout_i32 x0, layout_i32 y0,
                            layout_i32 x1, layout_i32 y1) {
    g_info.try_valid = valid;
    g_info.try_x0 = x0;
    g_info.try_y0 = y0;
    g_info.try_x1 = x1;
    g_info.try_y1 = y1;
}

static void set_try_preview_from_area(layout_i32 area_index) {
    if (!g_info.initialized) return;
    if (area_index < 0 || area_index >= g_info.area_count) return;
    LayoutArea *a = &g_info.areas[area_index];
    set_try_preview(1, a->x0, a->y0, a->x1, a->y1);
}

/* ── Initialization ────────────────────────────────────────────────── */

LAYOUT_EXPORT("init_screen")
layout_i32 init_screen(layout_i32 w, layout_i32 h, layout_i32 handle_size,
                        layout_i32 min_panel_size) {
    if (w <= 0 || h <= 0 || handle_size <= 0 || min_panel_size <= 0) {
        set_error(LAYOUT_ERR_INVALID_ARG);
        return LAYOUT_ERR_INVALID_ARG;
    }

    /* Zero the whole struct first. */
    layout_i32 *p = (layout_i32 *)&g_info;
    layout_i32 words = (layout_i32)(sizeof(g_info) / sizeof(layout_i32));
    for (layout_i32 i = 0; i < words; i++) p[i] = 0;

    /* Also zero the parallel handle metadata arrays. */
    for (layout_i32 i = 0; i < MAX_HANDLES; i++) {
        g_handle_axis[i]    = 0;
        g_handle_col_x0[i]  = 0;
        g_handle_col_x1[i]  = 0;
        g_handle_row_y0[i]  = 0;
        g_handle_row_y1[i]  = 0;
    }

    g_info.initialized      = 1;
    g_info.screen_w         = w;
    g_info.screen_h         = h;
    g_info.max_panels       = MAX_PANELS;
    g_info.max_handles      = MAX_HANDLES;
    g_info.handle_half_size = handle_size;   /* stored as passed (half-size semantics used at render) */
    g_info.min_panel_size   = min_panel_size;
    g_info.generation       = 1;

    /* One full-screen area. */
    g_info.area_count = 1;
    g_info.areas[0].x0 = 0;
    g_info.areas[0].y0 = 0;
    g_info.areas[0].x1 = w;
    g_info.areas[0].y1 = h;
    g_info.areas[0].content_id = 0;

    g_info.handle_count = 0;

    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

/* ── Resize screen ─────────────────────────────────────────────────── */

LAYOUT_EXPORT("resize_screen")
layout_i32 resize_screen(layout_i32 w, layout_i32 h, layout_i32 handle_size) {
    if (!g_info.initialized) {
        set_error(LAYOUT_ERR_NOT_INITIALIZED);
        return LAYOUT_ERR_NOT_INITIALIZED;
    }
    if (w <= 0 || h <= 0 || handle_size <= 0) {
        set_error(LAYOUT_ERR_INVALID_ARG);
        return LAYOUT_ERR_INVALID_ARG;
    }

    layout_i32 old_w = g_info.screen_w;
    layout_i32 old_h = g_info.screen_h;

    /* Scale all area coordinates. */
    for (layout_i32 i = 0; i < g_info.area_count; i++) {
        g_info.areas[i].x0 = scale_i32(g_info.areas[i].x0, w, old_w);
        g_info.areas[i].y0 = scale_i32(g_info.areas[i].y0, h, old_h);
        g_info.areas[i].x1 = scale_i32(g_info.areas[i].x1, w, old_w);
        g_info.areas[i].y1 = scale_i32(g_info.areas[i].y1, h, old_h);
    }

    /*
     * Scale handle logical boundary positions.
     *
     * For vertical handles, the logical boundary is midpoint x.
     * For horizontal handles, the logical boundary is midpoint y and
     * the column walls are g_handle_col_x0/x1.
     *
     * Scaling raw rect endpoints directly can shift midpoint by 1px due
     * to integer truncation. That breaks boundary matching against area
     * edges (a->x0/x1 or a->y0/y1), causing invalid/inverted handle spans.
     */
    for (layout_i32 i = 0; i < g_info.handle_count; i++) {
        LayoutHandle *hnd = &g_info.handles[i];
        if (g_handle_axis[i] == AXIS_VERTICAL) {
            layout_i32 old_bx = (hnd->x0 + hnd->x1) / 2;
            layout_i32 new_bx = scale_i32(old_bx, w, old_w);
            hnd->x0 = new_bx;
            hnd->x1 = new_bx;
            hnd->y0 = scale_i32(hnd->y0, h, old_h);
            hnd->y1 = scale_i32(hnd->y1, h, old_h);
            g_handle_row_y0[i] = scale_i32(g_handle_row_y0[i], h, old_h);
            g_handle_row_y1[i] = scale_i32(g_handle_row_y1[i], h, old_h);
        } else {
            layout_i32 old_by = (hnd->y0 + hnd->y1) / 2;
            layout_i32 new_by = scale_i32(old_by, h, old_h);
            hnd->y0 = new_by;
            hnd->y1 = new_by;
            hnd->x0 = scale_i32(hnd->x0, w, old_w);
            hnd->x1 = scale_i32(hnd->x1, w, old_w);
            g_handle_col_x0[i] = scale_i32(g_handle_col_x0[i], w, old_w);
            g_handle_col_x1[i] = scale_i32(g_handle_col_x1[i], w, old_w);
        }
    }

    g_info.screen_w         = w;
    g_info.screen_h         = h;
    g_info.handle_half_size = handle_size;

    /*
     * Handle thickness is derived from handle_half_size and needs a
     * recompute pass so existing handles snap to the new size.
     */
    for (layout_i32 i = 0; i < g_info.handle_count; i++) {
        recompute_handle_rect_scoped(i);
    }

    bump_generation();
    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

/* ── Handle move ───────────────────────────────────────────────────── */

/*
 * Recompute the bounding rectangle of handle[idx] from the current
 * areas.  A vertical handle at x=X spans y from the minimum y0 to the
 * maximum y1 of all areas that share that boundary.  Likewise for
 * horizontal handles.
 *
 * This is called after every successful mutation so that handle rects
 * stay consistent with the area layout.
 */
static void recompute_handle_rect(layout_i32 idx) {
    LayoutHandle *hnd = &g_info.handles[idx];
    layout_i32 axis   = g_handle_axis[idx];
    layout_i32 hs     = g_info.handle_half_size;

    if (axis == AXIS_VERTICAL) {
        /* Boundary x-coordinate is the midpoint of the handle rect. */
        layout_i32 bx = (hnd->x0 + hnd->x1) / 2;

        /* Find the span: min y0 and max y1 among areas touching bx. */
        layout_i32 span_y0 = g_info.screen_h;
        layout_i32 span_y1 = 0;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->x0 == bx || a->x1 == bx) {
                if (a->y0 < span_y0) span_y0 = a->y0;
                if (a->y1 > span_y1) span_y1 = a->y1;
            }
        }
        hnd->x0 = bx - hs;
        hnd->x1 = bx + hs;
        hnd->y0 = span_y0;
        hnd->y1 = span_y1;
    } else {
        /* Horizontal handle — boundary y-coordinate. */
        layout_i32 by = (hnd->y0 + hnd->y1) / 2;

        layout_i32 span_x0 = g_info.screen_w;
        layout_i32 span_x1 = 0;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->y0 == by || a->y1 == by) {
                if (a->x0 < span_x0) span_x0 = a->x0;
                if (a->x1 > span_x1) span_x1 = a->x1;
            }
        }
        hnd->y0 = by - hs;
        hnd->y1 = by + hs;
        hnd->x0 = span_x0;
        hnd->x1 = span_x1;
    }
}

/*
 * Recompute handle rect using per-handle scope metadata.
 *
 * Vertical handles are restricted to their row [row_y0, row_y1].
 * Horizontal handles are restricted to their column [col_x0, col_x1].
 *
 * This prevents independent handles from "sticking" together when they
 * align on the same boundary coordinate in different rows/columns.
 */
static void recompute_handle_rect_scoped(layout_i32 idx) {
    LayoutHandle *hnd = &g_info.handles[idx];
    layout_i32 axis   = g_handle_axis[idx];
    layout_i32 hs     = g_info.handle_half_size;

    if (axis == AXIS_VERTICAL) {
        layout_i32 bx = (hnd->x0 + hnd->x1) / 2;
        layout_i32 row_y0 = g_handle_row_y0[idx];
        layout_i32 row_y1 = g_handle_row_y1[idx];
        layout_i32 span_y0;
        layout_i32 span_y1;
        layout_i32 matched = 0;

        if (row_y0 > row_y1) {
            layout_i32 tmp = row_y0;
            row_y0 = row_y1;
            row_y1 = tmp;
        }
        if (row_y0 < 0) row_y0 = 0;
        if (row_y1 > g_info.screen_h) row_y1 = g_info.screen_h;

        span_y0 = row_y1;
        span_y1 = row_y0;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->y0 < row_y0 || a->y1 > row_y1) continue;
            if (a->x0 == bx || a->x1 == bx) {
                matched = 1;
                if (a->y0 < span_y0) span_y0 = a->y0;
                if (a->y1 > span_y1) span_y1 = a->y1;
            }
        }
        if (!matched || span_y0 > span_y1) {
            span_y0 = row_y0;
            span_y1 = row_y1;
        }
        hnd->x0 = bx - hs;
        hnd->x1 = bx + hs;
        hnd->y0 = span_y0;
        hnd->y1 = span_y1;
    } else {
        /*
         * Horizontal handle: span is restricted to its column.
         *
         * The column is defined by g_handle_col_x0/x1 — the x-coordinates
         * of the vertical walls bounding this handle.  These are kept
         * up-to-date by move_handle (vertical) so they always reflect the
         * current boundary positions, even after vertical handles move.
         *
         * We must NOT derive the column from the handle's own x0/x1 rect
         * fields, because those are the rendered span (which we are about
         * to recompute) and would be stale after a vertical boundary move.
         */
        layout_i32 col_x0 = g_handle_col_x0[idx];
        layout_i32 col_x1 = g_handle_col_x1[idx];
        layout_i32 matched = 0;
        if (col_x0 > col_x1) {
            layout_i32 tmp = col_x0;
            col_x0 = col_x1;
            col_x1 = tmp;
        }
        if (col_x0 < 0) col_x0 = 0;
        if (col_x1 > g_info.screen_w) col_x1 = g_info.screen_w;
        layout_i32 by     = (hnd->y0 + hnd->y1) / 2;

        layout_i32 span_x0 = col_x1; /* start at far end, narrow inward */
        layout_i32 span_x1 = col_x0;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            /* Area must be within the column and touch the boundary. */
            if (a->x0 >= col_x0 && a->x1 <= col_x1 &&
                (a->y0 == by || a->y1 == by)) {
                matched = 1;
                if (a->x0 < span_x0) span_x0 = a->x0;
                if (a->x1 > span_x1) span_x1 = a->x1;
            }
        }
        if (!matched || span_x0 > span_x1) {
            span_x0 = col_x0;
            span_x1 = col_x1;
        }
        hnd->y0 = by - hs;
        hnd->y1 = by + hs;
        hnd->x0 = span_x0;
        hnd->x1 = span_x1;
    }
}

LAYOUT_EXPORT("move_handle")
layout_i32 move_handle(layout_i32 handle_index, layout_i32 x, layout_i32 y) {
    if (!g_info.initialized) {
        set_error(LAYOUT_ERR_NOT_INITIALIZED);
        return LAYOUT_ERR_NOT_INITIALIZED;
    }
    if (handle_index < 0 || handle_index >= g_info.handle_count) {
        set_error(LAYOUT_ERR_INVALID_HANDLE);
        return LAYOUT_ERR_INVALID_HANDLE;
    }
    if (x < 0 || x > g_info.screen_w || y < 0 || y > g_info.screen_h) {
        set_error(LAYOUT_ERR_OUT_OF_BOUNDS);
        return LAYOUT_ERR_OUT_OF_BOUNDS;
    }

    layout_i32 axis = g_handle_axis[handle_index];
    layout_i32 hs   = g_info.handle_half_size;
    layout_i32 min  = g_info.min_panel_size;

    if (axis == AXIS_VERTICAL) {
        /*
         * Moving a vertical boundary to x.
         * Find the current boundary x from the handle rect midpoint.
         */
        layout_i32 old_bx = (g_info.handles[handle_index].x0 +
                              g_info.handles[handle_index].x1) / 2;
        layout_i32 row_y0 = g_handle_row_y0[handle_index];
        layout_i32 row_y1 = g_handle_row_y1[handle_index];

        if (row_y0 > row_y1) {
            layout_i32 tmp = row_y0;
            row_y0 = row_y1;
            row_y1 = tmp;
        }
        if (row_y0 < 0) row_y0 = 0;
        if (row_y1 > g_info.screen_h) row_y1 = g_info.screen_h;

        /* Clamp: all left areas must remain >= min wide,
         *        all right areas must remain >= min wide. */
        layout_i32 clamp_lo = 0;
        layout_i32 clamp_hi = g_info.screen_w;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->y0 < row_y0 || a->y1 > row_y1) continue;
            if (a->x1 == old_bx) {
                /* Left area: new x1 = x, must be >= a->x0 + min */
                layout_i32 lo = a->x0 + min;
                if (lo > clamp_lo) clamp_lo = lo;
            }
            if (a->x0 == old_bx) {
                /* Right area: new x0 = x, must be <= a->x1 - min */
                layout_i32 hi = a->x1 - min;
                if (hi < clamp_hi) clamp_hi = hi;
            }
        }
        if (x < clamp_lo) x = clamp_lo;
        if (x > clamp_hi) x = clamp_hi;

        /* Apply to all areas touching old_bx. */
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->y0 < row_y0 || a->y1 > row_y1) continue;
            if (a->x1 == old_bx) a->x1 = x;
            if (a->x0 == old_bx) a->x0 = x;
        }

        /*
         * Update column bounds for any horizontal handle whose left or
         * right wall was this vertical boundary.  This keeps g_handle_col_x0/x1
         * in sync so recompute_handle_rect_scoped can filter correctly.
         */
        for (layout_i32 i = 0; i < g_info.handle_count; i++) {
            layout_i32 by;
            if (g_handle_axis[i] != AXIS_HORIZONTAL) continue;
            by = (g_info.handles[i].y0 + g_info.handles[i].y1) / 2;
            if (by < row_y0 || by > row_y1) continue;
            if (g_handle_col_x0[i] == old_bx) g_handle_col_x0[i] = x;
            if (g_handle_col_x1[i] == old_bx) g_handle_col_x1[i] = x;
        }

        /* Update this handle's rect. */
        g_info.handles[handle_index].x0 = x - hs;
        g_info.handles[handle_index].x1 = x + hs;

    } else {
        /* Horizontal boundary to y. */
        layout_i32 old_by = (g_info.handles[handle_index].y0 +
                              g_info.handles[handle_index].y1) / 2;

        /* Column bounds for this handle — use explicit metadata, not the rect. */
        layout_i32 col_x0 = g_handle_col_x0[handle_index];
        layout_i32 col_x1 = g_handle_col_x1[handle_index];
        if (col_x0 > col_x1) {
            layout_i32 tmp = col_x0;
            col_x0 = col_x1;
            col_x1 = tmp;
        }
        if (col_x0 < 0) col_x0 = 0;
        if (col_x1 > g_info.screen_w) col_x1 = g_info.screen_w;

        layout_i32 clamp_lo = 0;
        layout_i32 clamp_hi = g_info.screen_h;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->x0 < col_x0 || a->x1 > col_x1) continue; /* outside column */
            if (a->y1 == old_by) {
                layout_i32 lo = a->y0 + min;
                if (lo > clamp_lo) clamp_lo = lo;
            }
            if (a->y0 == old_by) {
                layout_i32 hi = a->y1 - min;
                if (hi < clamp_hi) clamp_hi = hi;
            }
        }
        if (y < clamp_lo) y = clamp_lo;
        if (y > clamp_hi) y = clamp_hi;

        /* Apply only to areas within the column. */
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            LayoutArea *a = &g_info.areas[i];
            if (a->x0 < col_x0 || a->x1 > col_x1) continue;
            if (a->y1 == old_by) a->y1 = y;
            if (a->y0 == old_by) a->y0 = y;
        }

        /*
         * Update row bounds for any vertical handle whose top or bottom
         * wall was this horizontal boundary in this column range.
         */
        for (layout_i32 i = 0; i < g_info.handle_count; i++) {
            layout_i32 bx;
            if (g_handle_axis[i] != AXIS_VERTICAL) continue;
            bx = (g_info.handles[i].x0 + g_info.handles[i].x1) / 2;
            if (bx < col_x0 || bx > col_x1) continue;
            if (g_handle_row_y0[i] == old_by) g_handle_row_y0[i] = y;
            if (g_handle_row_y1[i] == old_by) g_handle_row_y1[i] = y;
        }

        /* Update this handle's rect. */
        g_info.handles[handle_index].y0 = y - hs;
        g_info.handles[handle_index].y1 = y + hs;
    }

    /*
     * Recompute ALL handle rects after areas have changed.
     * A single boundary move can affect the y-span of perpendicular
     * handles (e.g. moving a horizontal boundary changes the y-extent
     * of vertical handles that terminate at that boundary).
     */
    for (layout_i32 i = 0; i < g_info.handle_count; i++) {
        recompute_handle_rect_scoped(i);
    }

    bump_generation();
    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

/* ── Corner drag (split / merge) ───────────────────────────────────── */

/*
 * Determine whether point (px, py) is strictly inside area bounds.
 * "Inside" means x0 <= px < x1 and y0 <= py < y1.
 */
static layout_i32 point_in_area(layout_i32 ai, layout_i32 px, layout_i32 py) {
    LayoutArea *a = &g_info.areas[ai];
    return (px >= a->x0 && px < a->x1 && py >= a->y0 && py < a->y1);
}

/*
 * Remove area at index [dead] by shifting everything above it down.
 * Updates area_count.
 */
static void remove_area(layout_i32 dead) {
    for (layout_i32 i = dead; i < g_info.area_count - 1; i++) {
        g_info.areas[i] = g_info.areas[i + 1];
    }
    g_info.area_count--;
}

/*
 * Remove handle at index [dead] by shifting everything above it down.
 * Also shifts the parallel metadata arrays.
 * Updates handle_count.
 */
static void remove_handle(layout_i32 dead) {
    for (layout_i32 i = dead; i < g_info.handle_count - 1; i++) {
        g_info.handles[i]      = g_info.handles[i + 1];
        g_handle_axis[i]       = g_handle_axis[i + 1];
        g_handle_col_x0[i]     = g_handle_col_x0[i + 1];
        g_handle_col_x1[i]     = g_handle_col_x1[i + 1];
        g_handle_row_y0[i]     = g_handle_row_y0[i + 1];
        g_handle_row_y1[i]     = g_handle_row_y1[i + 1];
    }
    g_info.handle_count--;
}

/*
 * Try a simple merge of area [src_idx] into area [tgt_idx].
 *
 * "Simple" means the two areas share a complete edge — their shared
 * boundary spans the full extent of both areas on the perpendicular
 * axis.  No other area needs to be split or resized.
 *
 * Vertical boundary (src to the left of tgt, or vice versa):
 *   src.x1 == tgt.x0  (or tgt.x1 == src.x0)
 *   src.y0 == tgt.y0  AND  src.y1 == tgt.y1
 *
 * Horizontal boundary (src above tgt, or vice versa):
 *   src.y1 == tgt.y0  (or tgt.y1 == src.y0)
 *   src.x0 == tgt.x0  AND  src.x1 == tgt.x1
 *
 * Returns LAYOUT_OK on success (state mutated).
 * Returns LAYOUT_ERR_NOT_IMPLEMENTED if the pair is not simply-mergeable.
 */
static layout_i32 try_simple_merge(layout_i32 src_idx, layout_i32 tgt_idx) {
    LayoutArea *src = &g_info.areas[src_idx];
    LayoutArea *tgt = &g_info.areas[tgt_idx];

    layout_i32 boundary_axis = -1;
    layout_i32 boundary_val  = -1;
    /* left = the area whose right edge is the boundary */
    layout_i32 left_idx = -1, right_idx = -1;
    /* top  = the area whose bottom edge is the boundary */
    layout_i32 top_idx  = -1, bot_idx   = -1;

    /* Check vertical shared edge. */
    if (src->x1 == tgt->x0 && src->y0 == tgt->y0 && src->y1 == tgt->y1) {
        boundary_axis = AXIS_VERTICAL;
        boundary_val  = src->x1;
        left_idx = src_idx; right_idx = tgt_idx;
    } else if (tgt->x1 == src->x0 && tgt->y0 == src->y0 && tgt->y1 == src->y1) {
        boundary_axis = AXIS_VERTICAL;
        boundary_val  = tgt->x1;
        left_idx = tgt_idx; right_idx = src_idx;
    }
    /* Check horizontal shared edge. */
    else if (src->y1 == tgt->y0 && src->x0 == tgt->x0 && src->x1 == tgt->x1) {
        boundary_axis = AXIS_HORIZONTAL;
        boundary_val  = src->y1;
        top_idx = src_idx; bot_idx = tgt_idx;
    } else if (tgt->y1 == src->y0 && tgt->x0 == src->x0 && tgt->x1 == src->x1) {
        boundary_axis = AXIS_HORIZONTAL;
        boundary_val  = tgt->y1;
        top_idx = tgt_idx; bot_idx = src_idx;
    }

    if (boundary_axis == -1) {
        /* Not simply-mergeable. */
        return LAYOUT_ERR_NOT_IMPLEMENTED;
    }

    /*
     * Find the handle that sits on this boundary.
     * For a vertical boundary at x=V: handle is AXIS_VERTICAL with midpoint x==V.
     * For a horizontal boundary at y=V: handle is AXIS_HORIZONTAL with midpoint y==V
     *   AND its column contains both areas.
     */
    layout_i32 hnd_idx = -1;
    for (layout_i32 i = 0; i < g_info.handle_count; i++) {
        if (g_handle_axis[i] == boundary_axis) {
            if (boundary_axis == AXIS_VERTICAL) {
                layout_i32 bx = (g_info.handles[i].x0 + g_info.handles[i].x1) / 2;
                if (bx == boundary_val) { hnd_idx = i; break; }
            } else {
                layout_i32 by = (g_info.handles[i].y0 + g_info.handles[i].y1) / 2;
                if (by == boundary_val) { hnd_idx = i; break; }
            }
        }
    }
    /* No matching handle is a logic error, but treat gracefully. */
    if (hnd_idx == -1) {
        return LAYOUT_ERR_NOT_IMPLEMENTED;
    }

    /*
     * Guard: the handle's span must match exactly the merging areas' extent.
     *
     * A vertical handle at x=V may span more than just the two merging
     * areas if other areas also border x=V (e.g. a "+" layout where the
     * vertical boundary continues into a row below).  Removing the handle
     * in that case would leave those other areas with no handle between
     * them — a broken state.
     *
     * The handle's current span (from recompute_handle_rect_scoped) reflects
     * all areas touching the boundary, so we just compare it to the merging
     * areas' extent.
     */
    if (boundary_axis == AXIS_VERTICAL) {
        /* Merging areas share y0..y1; handle must span exactly that range. */
        layout_i32 shared_y0 = g_info.areas[left_idx].y0;
        layout_i32 shared_y1 = g_info.areas[left_idx].y1;
        if (g_info.handles[hnd_idx].y0 != shared_y0 ||
            g_info.handles[hnd_idx].y1 != shared_y1) {
            return LAYOUT_ERR_NOT_IMPLEMENTED;
        }
    } else {
        /* Merging areas share x0..x1; handle must span exactly that range. */
        layout_i32 shared_x0 = g_info.areas[top_idx].x0;
        layout_i32 shared_x1 = g_info.areas[top_idx].x1;
        if (g_info.handles[hnd_idx].x0 != shared_x0 ||
            g_info.handles[hnd_idx].x1 != shared_x1) {
            return LAYOUT_ERR_NOT_IMPLEMENTED;
        }
    }

    /*
     * Perform the merge.
     * Lower index survives; it absorbs the other area's extent.
     * Surviving area keeps its own content_id (spec: "surviving area keeps its ID").
     */
    layout_i32 survivor, dead;
    if (boundary_axis == AXIS_VERTICAL) {
        survivor = left_idx < right_idx ? left_idx : right_idx;
        dead     = left_idx < right_idx ? right_idx : left_idx;
        /* Expand survivor to cover both. */
        g_info.areas[survivor].x0 = g_info.areas[left_idx].x0;
        g_info.areas[survivor].y0 = g_info.areas[left_idx].y0;
        g_info.areas[survivor].x1 = g_info.areas[right_idx].x1;
        g_info.areas[survivor].y1 = g_info.areas[right_idx].y1;
    } else {
        survivor = top_idx < bot_idx ? top_idx : bot_idx;
        dead     = top_idx < bot_idx ? bot_idx : top_idx;
        g_info.areas[survivor].x0 = g_info.areas[top_idx].x0;
        g_info.areas[survivor].y0 = g_info.areas[top_idx].y0;
        g_info.areas[survivor].x1 = g_info.areas[bot_idx].x1;
        g_info.areas[survivor].y1 = g_info.areas[bot_idx].y1;
    }

    /* Remove the dead area and the shared handle. */
    remove_area(dead);
    remove_handle(hnd_idx);

    /* Recompute all remaining handle rects (spans may have changed). */
    for (layout_i32 i = 0; i < g_info.handle_count; i++) {
        recompute_handle_rect_scoped(i);
    }

    return LAYOUT_OK;
}

static layout_i32 try_simple_merge_preview(layout_i32 src_idx, layout_i32 tgt_idx,
                                           layout_i32 *out_x0, layout_i32 *out_y0,
                                           layout_i32 *out_x1, layout_i32 *out_y1) {
    LayoutArea *src = &g_info.areas[src_idx];
    LayoutArea *tgt = &g_info.areas[tgt_idx];

    layout_i32 boundary_axis = -1;
    layout_i32 boundary_val  = -1;
    layout_i32 left_idx = -1, right_idx = -1;
    layout_i32 top_idx  = -1, bot_idx   = -1;

    if (src->x1 == tgt->x0 && src->y0 == tgt->y0 && src->y1 == tgt->y1) {
        boundary_axis = AXIS_VERTICAL;
        boundary_val  = src->x1;
        left_idx = src_idx; right_idx = tgt_idx;
    } else if (tgt->x1 == src->x0 && tgt->y0 == src->y0 && tgt->y1 == src->y1) {
        boundary_axis = AXIS_VERTICAL;
        boundary_val  = tgt->x1;
        left_idx = tgt_idx; right_idx = src_idx;
    } else if (src->y1 == tgt->y0 && src->x0 == tgt->x0 && src->x1 == tgt->x1) {
        boundary_axis = AXIS_HORIZONTAL;
        boundary_val  = src->y1;
        top_idx = src_idx; bot_idx = tgt_idx;
    } else if (tgt->y1 == src->y0 && tgt->x0 == src->x0 && tgt->x1 == src->x1) {
        boundary_axis = AXIS_HORIZONTAL;
        boundary_val  = tgt->y1;
        top_idx = tgt_idx; bot_idx = src_idx;
    }

    if (boundary_axis == -1) {
        return LAYOUT_ERR_NOT_IMPLEMENTED;
    }

    layout_i32 hnd_idx = -1;
    for (layout_i32 i = 0; i < g_info.handle_count; i++) {
        if (g_handle_axis[i] == boundary_axis) {
            if (boundary_axis == AXIS_VERTICAL) {
                layout_i32 bx = (g_info.handles[i].x0 + g_info.handles[i].x1) / 2;
                if (bx == boundary_val) { hnd_idx = i; break; }
            } else {
                layout_i32 by = (g_info.handles[i].y0 + g_info.handles[i].y1) / 2;
                if (by == boundary_val) { hnd_idx = i; break; }
            }
        }
    }
    if (hnd_idx == -1) {
        return LAYOUT_ERR_NOT_IMPLEMENTED;
    }

    if (boundary_axis == AXIS_VERTICAL) {
        layout_i32 shared_y0 = g_info.areas[left_idx].y0;
        layout_i32 shared_y1 = g_info.areas[left_idx].y1;
        if (g_info.handles[hnd_idx].y0 != shared_y0 ||
            g_info.handles[hnd_idx].y1 != shared_y1) {
            return LAYOUT_ERR_NOT_IMPLEMENTED;
        }
        *out_x0 = g_info.areas[left_idx].x0;
        *out_y0 = g_info.areas[left_idx].y0;
        *out_x1 = g_info.areas[right_idx].x1;
        *out_y1 = g_info.areas[right_idx].y1;
    } else {
        layout_i32 shared_x0 = g_info.areas[top_idx].x0;
        layout_i32 shared_x1 = g_info.areas[top_idx].x1;
        if (g_info.handles[hnd_idx].x0 != shared_x0 ||
            g_info.handles[hnd_idx].x1 != shared_x1) {
            return LAYOUT_ERR_NOT_IMPLEMENTED;
        }
        *out_x0 = g_info.areas[top_idx].x0;
        *out_y0 = g_info.areas[top_idx].y0;
        *out_x1 = g_info.areas[bot_idx].x1;
        *out_y1 = g_info.areas[bot_idx].y1;
    }

    return LAYOUT_OK;
}

LAYOUT_EXPORT("try_corner")
layout_i32 try_corner(layout_i32 area_index, layout_i32 corner_index,
                      layout_i32 x, layout_i32 y) {
    if (!g_info.initialized) {
        set_error(LAYOUT_ERR_NOT_INITIALIZED);
        return LAYOUT_ERR_NOT_INITIALIZED;
    }
    if (area_index < 0 || area_index >= g_info.area_count) {
        set_error(LAYOUT_ERR_INVALID_AREA);
        return LAYOUT_ERR_INVALID_AREA;
    }
    if (corner_index < 0 || corner_index > 3) {
        set_try_preview_from_area(area_index);
        set_error(LAYOUT_ERR_INVALID_CORNER);
        return LAYOUT_ERR_INVALID_CORNER;
    }
    if (x < 0 || x >= g_info.screen_w || y < 0 || y >= g_info.screen_h) {
        set_try_preview_from_area(area_index);
        set_error(LAYOUT_ERR_OUT_OF_BOUNDS);
        return LAYOUT_ERR_OUT_OF_BOUNDS;
    }

    LayoutArea *src = &g_info.areas[area_index];

    if (!point_in_area(area_index, x, y)) {
        layout_i32 tgt_idx = -1;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            if (i == area_index) continue;
            if (point_in_area(i, x, y)) { tgt_idx = i; break; }
        }
        if (tgt_idx == -1) {
            set_try_preview_from_area(area_index);
            set_error(LAYOUT_ERR_OUT_OF_BOUNDS);
            return LAYOUT_ERR_OUT_OF_BOUNDS;
        }

        layout_i32 px0, py0, px1, py1;
        layout_i32 err = try_simple_merge_preview(area_index, tgt_idx, &px0, &py0, &px1, &py1);
        if (err == LAYOUT_OK) {
            set_try_preview(1, px0, py0, px1, py1);
        } else {
            set_try_preview_from_area(area_index);
        }
        set_error(err);
        return err;
    }

    if (g_info.area_count >= MAX_PANELS) {
        set_try_preview_from_area(area_index);
        set_error(LAYOUT_ERR_CAPACITY);
        return LAYOUT_ERR_CAPACITY;
    }
    if (g_info.handle_count >= MAX_HANDLES) {
        set_try_preview_from_area(area_index);
        set_error(LAYOUT_ERR_CAPACITY);
        return LAYOUT_ERR_CAPACITY;
    }

    layout_i32 cx, cy;
    switch (corner_index) {
        case 0: cx = src->x0; cy = src->y0; break;
        case 1: cx = src->x1; cy = src->y0; break;
        case 2: cx = src->x1; cy = src->y1; break;
        case 3: cx = src->x0; cy = src->y1; break;
        default: cx = 0; cy = 0; break;
    }

    layout_i32 dx = abs_i32(x - cx);
    layout_i32 dy = abs_i32(y - cy);
    layout_i32 axis = (dy > dx) ? AXIS_HORIZONTAL : AXIS_VERTICAL;
    layout_i32 min = g_info.min_panel_size;

    layout_i32 sx0 = src->x0, sy0 = src->y0;
    layout_i32 sx1 = src->x1, sy1 = src->y1;

    if (axis == AXIS_VERTICAL) {
        layout_i32 split_x = x;
        if (split_x - sx0 < min) split_x = sx0 + min;
        if (sx1 - split_x < min) split_x = sx1 - min;
        if (split_x <= sx0 || split_x >= sx1) {
            set_try_preview_from_area(area_index);
            set_error(LAYOUT_ERR_MIN_SIZE);
            return LAYOUT_ERR_MIN_SIZE;
        }

        if (corner_index == 0 || corner_index == 3) {
            set_try_preview(1, sx0, sy0, split_x, sy1);
        } else {
            set_try_preview(1, split_x, sy0, sx1, sy1);
        }
    } else {
        layout_i32 split_y = y;
        if (split_y - sy0 < min) split_y = sy0 + min;
        if (sy1 - split_y < min) split_y = sy1 - min;
        if (split_y <= sy0 || split_y >= sy1) {
            set_try_preview_from_area(area_index);
            set_error(LAYOUT_ERR_MIN_SIZE);
            return LAYOUT_ERR_MIN_SIZE;
        }

        if (corner_index == 0 || corner_index == 1) {
            set_try_preview(1, sx0, sy0, sx1, split_y);
        } else {
            set_try_preview(1, sx0, split_y, sx1, sy1);
        }
    }

    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

LAYOUT_EXPORT("move_corner")
layout_i32 move_corner(layout_i32 area_index, layout_i32 corner_index,
                        layout_i32 x, layout_i32 y) {
    if (!g_info.initialized) {
        set_error(LAYOUT_ERR_NOT_INITIALIZED);
        return LAYOUT_ERR_NOT_INITIALIZED;
    }
    if (area_index < 0 || area_index >= g_info.area_count) {
        set_error(LAYOUT_ERR_INVALID_AREA);
        return LAYOUT_ERR_INVALID_AREA;
    }
    if (corner_index < 0 || corner_index > 3) {
        set_error(LAYOUT_ERR_INVALID_CORNER);
        return LAYOUT_ERR_INVALID_CORNER;
    }
    if (x < 0 || x >= g_info.screen_w || y < 0 || y >= g_info.screen_h) {
        set_error(LAYOUT_ERR_OUT_OF_BOUNDS);
        return LAYOUT_ERR_OUT_OF_BOUNDS;
    }

    LayoutArea *src = &g_info.areas[area_index];

    /* Check if target is inside the source area. */
    if (!point_in_area(area_index, x, y)) {
        /*
         * Target is outside the source area — attempt a merge.
         * Find which area the drag landed in.
         */
        layout_i32 tgt_idx = -1;
        for (layout_i32 i = 0; i < g_info.area_count; i++) {
            if (i == area_index) continue;
            if (point_in_area(i, x, y)) { tgt_idx = i; break; }
        }
        if (tgt_idx == -1) {
            set_error(LAYOUT_ERR_OUT_OF_BOUNDS);
            return LAYOUT_ERR_OUT_OF_BOUNDS;
        }

        layout_i32 err = try_simple_merge(area_index, tgt_idx);
        set_error(err);
        if (err == LAYOUT_OK) bump_generation();
        return err;
    }

    /* ── Split ── */

    if (g_info.area_count >= MAX_PANELS) {
        set_error(LAYOUT_ERR_CAPACITY);
        return LAYOUT_ERR_CAPACITY;
    }
    if (g_info.handle_count >= MAX_HANDLES) {
        set_error(LAYOUT_ERR_CAPACITY);
        return LAYOUT_ERR_CAPACITY;
    }

    /*
     * Determine split axis from drag delta.
     * Corner positions:
     *   0 = TL (x0, y0)   1 = TR (x1, y0)
     *   2 = BR (x1, y1)   3 = BL (x0, y1)
     */
    layout_i32 cx, cy; /* corner pixel position */
    switch (corner_index) {
        case 0: cx = src->x0; cy = src->y0; break;
        case 1: cx = src->x1; cy = src->y0; break;
        case 2: cx = src->x1; cy = src->y1; break;
        case 3: cx = src->x0; cy = src->y1; break;
        default: cx = 0; cy = 0; break;
    }

    layout_i32 dx = abs_i32(x - cx);
    layout_i32 dy = abs_i32(y - cy);

    /*
     * Choose axis: whichever delta is larger → that axis dominates.
     * Tie-break: prefer vertical (dx == dy).
     */
    layout_i32 axis = (dy > dx) ? AXIS_HORIZONTAL : AXIS_VERTICAL;

    layout_i32 hs  = g_info.handle_half_size;
    layout_i32 min = g_info.min_panel_size;

    /* Snapshot src bounds before we modify anything. */
    layout_i32 sx0 = src->x0, sy0 = src->y0;
    layout_i32 sx1 = src->x1, sy1 = src->y1;
    layout_i32 scid = src->content_id;

    layout_i32 new_idx = g_info.area_count;
    layout_i32 hnd_idx = g_info.handle_count;

    if (axis == AXIS_VERTICAL) {
        /* Split at x = x (drag target x-coordinate). */
        layout_i32 split_x = x;

        /* Enforce minimum size on both resulting areas. */
        if (split_x - sx0 < min) split_x = sx0 + min;
        if (sx1 - split_x < min) split_x = sx1 - min;
        if (split_x <= sx0 || split_x >= sx1) {
            set_error(LAYOUT_ERR_MIN_SIZE);
            return LAYOUT_ERR_MIN_SIZE;
        }

        /* Resize source area to left half. */
        g_info.areas[area_index].x1 = split_x;

        /* Create right area. */
        g_info.areas[new_idx].x0 = split_x;
        g_info.areas[new_idx].y0 = sy0;
        g_info.areas[new_idx].x1 = sx1;
        g_info.areas[new_idx].y1 = sy1;
        g_info.areas[new_idx].content_id = scid;
        g_info.area_count++;

        /* Create vertical handle. */
        g_info.handles[hnd_idx].x0 = split_x - hs;
        g_info.handles[hnd_idx].y0 = sy0;
        g_info.handles[hnd_idx].x1 = split_x + hs;
        g_info.handles[hnd_idx].y1 = sy1;
        g_info.handles[hnd_idx].content_id = 0;
        g_handle_axis[hnd_idx] = AXIS_VERTICAL;
        g_handle_col_x0[hnd_idx] = split_x;
        g_handle_col_x1[hnd_idx] = split_x;
        g_handle_row_y0[hnd_idx] = sy0;
        g_handle_row_y1[hnd_idx] = sy1;
        g_info.handle_count++;

        /*
         * Recompute the span of this new vertical handle: it must span
         * the full height of all areas sharing the new boundary x.
         * (At creation time this is just sy0..sy1, but recompute for
         * correctness in case of future mutations.)
         */
        recompute_handle_rect_scoped(hnd_idx);

    } else {
        /* Split at y = y (drag target y-coordinate). */
        layout_i32 split_y = y;

        if (split_y - sy0 < min) split_y = sy0 + min;
        if (sy1 - split_y < min) split_y = sy1 - min;
        if (split_y <= sy0 || split_y >= sy1) {
            set_error(LAYOUT_ERR_MIN_SIZE);
            return LAYOUT_ERR_MIN_SIZE;
        }

        /* Resize source area to top half. */
        g_info.areas[area_index].y1 = split_y;

        /* Create bottom area. */
        g_info.areas[new_idx].x0 = sx0;
        g_info.areas[new_idx].y0 = split_y;
        g_info.areas[new_idx].x1 = sx1;
        g_info.areas[new_idx].y1 = sy1;
        g_info.areas[new_idx].content_id = scid;
        g_info.area_count++;

        /* Create horizontal handle. */
        g_info.handles[hnd_idx].x0 = sx0;
        g_info.handles[hnd_idx].y0 = split_y - hs;
        g_info.handles[hnd_idx].x1 = sx1;
        g_info.handles[hnd_idx].y1 = split_y + hs;
        g_info.handles[hnd_idx].content_id = 0;
        g_handle_axis[hnd_idx]   = AXIS_HORIZONTAL;
        g_handle_col_x0[hnd_idx] = sx0;  /* left wall of the column at split time */
        g_handle_col_x1[hnd_idx] = sx1;  /* right wall of the column at split time */
        g_handle_row_y0[hnd_idx] = split_y;
        g_handle_row_y1[hnd_idx] = split_y;
        g_info.handle_count++;

        recompute_handle_rect_scoped(hnd_idx);
    }

    bump_generation();
    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

/* ── Content assignment ────────────────────────────────────────────── */

LAYOUT_EXPORT("set_area_content")
layout_i32 set_area_content(layout_i32 area_index, layout_i32 content_id) {
    if (!g_info.initialized) {
        set_error(LAYOUT_ERR_NOT_INITIALIZED);
        return LAYOUT_ERR_NOT_INITIALIZED;
    }
    if (area_index < 0 || area_index >= g_info.area_count) {
        set_error(LAYOUT_ERR_INVALID_AREA);
        return LAYOUT_ERR_INVALID_AREA;
    }
    g_info.areas[area_index].content_id = content_id;
    bump_generation();
    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

LAYOUT_EXPORT("set_handle_content")
layout_i32 set_handle_content(layout_i32 handle_index, layout_i32 content_id) {
    if (!g_info.initialized) {
        set_error(LAYOUT_ERR_NOT_INITIALIZED);
        return LAYOUT_ERR_NOT_INITIALIZED;
    }
    if (handle_index < 0 || handle_index >= g_info.handle_count) {
        set_error(LAYOUT_ERR_INVALID_HANDLE);
        return LAYOUT_ERR_INVALID_HANDLE;
    }
    g_info.handles[handle_index].content_id = content_id;
    bump_generation();
    set_error(LAYOUT_OK);
    return LAYOUT_OK;
}

/* ── Snapshot pointer ──────────────────────────────────────────────── */

LAYOUT_EXPORT("get_info_ptr")
layout_i32 get_info_ptr(void) {
    return (layout_i32)(long)&g_info;
}
