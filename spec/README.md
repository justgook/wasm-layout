# Headless Layout Manager Specification (Blender-Inspired)

## Abstract

A headless layout manager for rectangular panel arrangements inspired by Blender’s screen system.  
It partitions a screen rectangle into a set of axis-aligned rectangular areas that exactly tile the space (no overlaps, no gaps).  
Users can split areas, merge areas (including non-siblings), and resize boundaries via handle dragging or corner dragging.

The system is designed for backend use (e.g., WebAssembly), exposing a minimal C API and a flat shared-memory snapshot for rendering by a host application.

---

## Goals

- Deterministic, backend-driven layout logic
- Blender-like editing semantics:
  - Split areas by dragging corners inward
  - Merge areas by dragging corners into neighbors (not limited to siblings)
  - Resize via draggable handles
  - Support complex merges that may split the target area
- No rendering responsibilities
- Small memory footprint with fixed capacity
- Host provides interaction intent (area index, corner index, coordinates)

---

## Non-Goals

- Rendering, hit-testing, or gesture detection
- Thread safety
- Floating panels or non-rectangular regions
- Arbitrary polygonal layouts (rectangles only)

---

## Core Model

### Screen

A root rectangle `[0, screen_w) × [0, screen_h)`.

### Area

A leaf rectangular region:

- Axis-aligned integer bounds `(x0, y0, x1, y1)`
- Opaque `content_id` assigned by host
- Areas exactly tile the screen

### Handle

An interactive divider corresponding to a boundary between two adjacent areas:

- Represented as a thin rectangle centered on the boundary
- Has an optional `content_id`
- Moving a handle shifts the boundary while preserving constraints

### Corner

Each area has four corners:

```
0 = Top-Left
1 = Top-Right
2 = Bottom-Right
3 = Bottom-Left
```

Corners are used to initiate split or merge operations.

---

## Layout Invariants

The layout must always satisfy:

1. **Exact Tiling**  
   Areas exactly cover the screen rectangle. No overlaps, no gaps.

2. **Rectangular Faces Only**  
   Every area is a rectangle.

3. **Axis Alignment**  
   All edges are horizontal or vertical.

4. **Minimum Size Constraint**  
   Every area width and height ≥ `min_panel_size`.

5. **Manifold Boundaries**  
   Interior edges separate exactly two areas.

6. **Capacity Limit**  
   Maximum number of areas and handles is fixed at compile time.

7. **Stable Snapshot Indices**  
   Area and handle indices remain valid until the next successful mutation.

---

## Interaction Model

The host determines which area/corner/handle is manipulated and calls the API.  
All layout decisions are made by the backend.

### Handle Drag (Resize)

Moves a boundary between areas.

**Input:** handle index, pointer position `(x, y)`

Behavior:

- Moves the associated boundary along its axis
- Adjacent areas expand/contract accordingly
- Movement is clamped to preserve minimum sizes
- May affect multiple areas if boundaries are collinear

---

### Corner Drag (Split or Merge)

Moves a specific area corner toward a target position.

**Input:** area index, corner index, pointer position `(x, y)`

The backend determines the operation class:

#### 1. Split (Corner → Internal Drop)

Triggered when the drag remains inside the source area.

Effects:

- Inserts one or two new boundaries
- Splits the source area into two rectangles
- Creates one new area
- Both resulting areas inherit the original content_id (host may change later)

Split axis and position are inferred from drag direction and corner.

---

#### 2. Merge / Steal (Corner → Neighbor Drop)

Triggered when the drag crosses into adjacent area(s).

Effects:

- Removes or repositions boundaries locally
- The dragged area expands into neighboring space
- The drop-target area may be split into multiple rectangles
  to preserve rectangular tiling

If an existing area is split due to the merge:
- All resulting pieces inherit the original area’s `content_id`

If areas merge into a single rectangle:
- The surviving area is deterministic (implementation-defined rule)

---

### Minimum Size Enforcement During Merge

All resulting areas must satisfy the minimum size constraint.

If a merge would produce an area smaller than `min_panel_size`:

1. The operation attempts to **adjust nearby boundaries** (handles)
   by the smallest amount necessary to restore valid sizes.

2. Boundary adjustments may propagate to adjacent areas along the
   same axis (pushing a chain of collinear edges).

3. Adjustments must preserve:
   - Exact tiling
   - Rectangular shapes
   - Screen bounds

4. If no valid configuration exists within constraints:
   - The operation is cancelled
   - Layout remains unchanged
   - An error is returned

This allows merges that would otherwise be invalid by slightly
rebalancing neighboring areas, matching Blender-like behavior.

---

### Survivability Rule

An area may only survive a merge if its resulting width and height
are both ≥ `min_panel_size`.

Areas that cannot satisfy the minimum size after all allowed
boundary adjustments are eliminated as part of the merge.

No operation may leave behind a sub-minimum “degenerate” area.
---

#### 3. Cancel

If constraints cannot be satisfied (minimum size, bounds, capacity),  
the layout remains unchanged and an error is reported.

---

## Resize Screen

Rescales the entire layout to a new screen size.

Behavior:

- All boundaries scale proportionally
- Relative layout structure is preserved
- Minimum size constraints are re-enforced

---

## Content IDs

- Assigned by the host
- Opaque to the layout manager
- Preserved unless areas are destroyed
- When an area is split, new areas inherit the original ID
- When areas merge, surviving area keeps its ID

---

## Snapshot Export

The layout manager exposes a flat snapshot structure in shared memory:

- Screen size
- Counts
- Generation counter
- Error status
- Arrays of areas and handles with rectangles and content IDs

The host reads this structure to render the layout.

---

## Generation Counter

Incremented on every successful mutation:

- Split
- Merge
- Handle move
- Resize
- Content assignment

Allows the host to detect layout changes efficiently.

---

## Error Handling

All API calls return an error code.

Common error cases:

- Not initialized
- Invalid indices
- Out-of-bounds operations
- Minimum size violation
- Capacity exceeded

Errors do not modify layout state.

---

## Determinism

Given identical sequences of API calls, the layout must evolve identically.  
No randomness or host-dependent behavior is allowed.

---

## Implementation Notes (Non-Normative)

Internally, the layout can be represented as any structure that preserves the invariants, such as:

- Orthogonal planar graph (rectangular mesh)
- Constraint-maintained boundary network
- Rebuilt topology per mutation

A binary split tree alone is insufficient to represent all supported merge operations.

---

## Summary

This system models a Blender-style rectangular tiling editor for headless environments:

- Rectangular areas that always tile the screen
- Handles for resizing
- Corner dragging for split and advanced merge operations
- Deterministic backend logic
- Minimal host responsibilities
- Flat snapshot output for rendering

The result is a compact, robust layout engine suitable for WebAssembly and other headless contexts.
