# ADR-001: Data Model — Flat Arrays with Explicit Handle Spans

**Status:** Accepted  
**Date:** 2026-02-20  
**Context:** Initial implementation of the headless layout manager

---

## Context

The spec requires a rectangular tiling layout engine that supports:
- Split (corner drag inward)
- Merge (corner drag into neighbor) — deferred
- Handle drag (resize boundary)
- Proportional resize of the whole screen

The spec notes that "a binary split tree alone is insufficient to represent all supported merge operations." We need a data model that can represent arbitrary rectangular tilings, not just those producible by recursive binary splits.

---

## Decision

### Primary representation: flat arrays inside `LayoutInfo`

Areas and handles are stored as flat arrays directly inside the `LayoutInfo` struct (which lives in WASM linear memory). This matches the ABI expected by the host (JavaScript reads the struct at the pointer returned by `get_info_ptr()`).

```
LayoutInfo {
  header[15]          // 15 × i32 metadata fields
  areas[MAX_PANELS]   // 16 × 5 i32 = 80 i32
  handles[MAX_HANDLES]// 15 × 5 i32 = 75 i32
}
```

Indices are stable until the next successful mutation (per spec invariant 7).

### Handle representation: explicit bounding rectangle + axis tag

Each handle stores:
- `(x0, y0, x1, y1)` — the rendered hit-test rectangle (centered on the boundary, ±`handle_half_size`)
- `content_id` — opaque host tag
- `axis` (internal, parallel array `g_handle_axis[]`) — `AXIS_VERTICAL` or `AXIS_HORIZONTAL`

The boundary coordinate is recovered as the midpoint of the handle rect:
- Vertical handle: `bx = (x0 + x1) / 2`
- Horizontal handle: `by = (y0 + y1) / 2`

### Handle span is column-scoped

A critical invariant: **horizontal handles must not span across vertical boundaries**.

When a column is split horizontally, the new horizontal handle spans only the width of that column (`x0..x1` of the split area). Vertical handles span the full height of all areas sharing that boundary.

This is enforced by `recompute_handle_rect_scoped()`:
- For vertical handles: scan all areas with `a->x0 == bx || a->x1 == bx`, take min/max y.
- For horizontal handles: scan only areas where `a->x0 >= col_x0 && a->x1 <= col_x1` (column bounds preserved from creation), take min/max x.

The column bounds for a horizontal handle are stored implicitly in its `x0`/`x1` fields (before the half-size offset is applied). This avoids a separate metadata field.

### Handle move is boundary-scoped

`move_handle` for a vertical boundary at `old_bx`:
- Updates `a->x1 = new_bx` for all areas where `a->x1 == old_bx`
- Updates `a->x0 = new_bx` for all areas where `a->x0 == old_bx`
- No column filtering — vertical boundaries span the full height

`move_handle` for a horizontal boundary at `old_by`:
- Filters to areas within the handle's column (`col_x0..col_x1`)
- Updates only `a->y0` / `a->y1` within that column

### Split axis selection

Given a corner drag from corner position `(cx, cy)` to target `(x, y)`:
- `dx = |x - cx|`, `dy = |y - cy|`
- If `dy > dx` → horizontal split at `y`
- Otherwise (including tie) → vertical split at `x`

The split position is the drag target coordinate (clamped to preserve `min_panel_size` on both resulting areas).

### Merge: deferred (NOT_IMPLEMENTED)

Merge requires detecting when a corner drag crosses into a neighboring area and computing a valid rectangular decomposition of the merged region. This is non-trivial and deferred to a later implementation phase. The API returns `LAYOUT_ERR_NOT_IMPLEMENTED` for cross-area corner drags.

---

## Alternatives Considered

### Binary split tree

**Rejected.** The spec explicitly states this is insufficient for the full merge semantics. A tree can only represent layouts producible by recursive binary splits; arbitrary merges can produce layouts that require a more general graph representation.

### Orthogonal planar graph (full mesh)

**Deferred.** A proper rectangular mesh (vertices at all boundary intersections, edges along boundaries) would be the most general representation and would make merge operations straightforward. However, it adds significant complexity for the initial implementation. The flat-array approach with explicit handle spans is sufficient for all split and resize operations and can be migrated to a mesh representation when merge is implemented.

### Storing column bounds as a separate field per handle

**Rejected in favor of implicit encoding.** The column bounds for a horizontal handle are exactly its `x0..x1` span (the rendered rectangle width). Storing them separately would duplicate data and risk inconsistency.

---

## Consequences

**Good:**
- Simple, cache-friendly data layout
- Direct ABI compatibility with the JavaScript host
- All current tests pass with this model
- Easy to reason about for split and resize operations

**Bad / Risks:**
- Merge operations will require either extending this model or migrating to a mesh representation
- The implicit column-bounds encoding in handle `x0/x1` is a subtle invariant that must be maintained carefully
- `recompute_handle_rect_scoped` is called after every mutation — O(n) per handle per mutation, acceptable for MAX_HANDLES=15

---

## Open Questions

1. **Merge implementation**: When merge is implemented, will the flat-array model be sufficient, or will we need a proper rectangular mesh? The spec's note about binary trees suggests the mesh approach may be necessary.

2. **Handle identity stability**: Currently handles are appended and never removed (no merge). When merge is implemented, handles may need to be removed, which will invalidate indices. The spec says indices are stable "until the next successful mutation" — this is already the contract.

3. **Collinear handle merging**: Should two horizontal handles at the same `y` in adjacent columns ever be merged into one spanning handle? The "sticky handles" regression test explicitly requires they do NOT merge. This is the correct behavior.
