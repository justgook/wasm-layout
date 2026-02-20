const fs = require("fs");
const path = require("path");

const ERR = {
  OK: 0,
  NOT_INITIALIZED: 1,
  INVALID_ARG: 2,
  INVALID_HANDLE: 3,
  INVALID_AREA: 4,
  INVALID_CORNER: 5,
  OUT_OF_BOUNDS: 6,
  MIN_SIZE: 7,
  NOT_IMPLEMENTED: 8,
  CAPACITY: 9,
};

const ABI = {
  HEADER_I32: 15,
  AREA_I32: 5,
  HANDLE_I32: 5,
  MAX_PANELS: 16,
};

const HANDLE_SIZE = 4;
const MIN_PANEL_SIZE = 16;

function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}. expected=${expected}, got=${actual}`);
  }
}

async function run() {
  const wasmPath = path.join(__dirname, "build.nosync/web/layout.wasm");
  if (!fs.existsSync(wasmPath)) {
    throw new Error(`Missing ${wasmPath}. Run: make web`);
  }

  const wasmBytes = fs.readFileSync(wasmPath);
  const memory = new WebAssembly.Memory({
    initial: 288,
    maximum: 512,
    shared: true,
  });

  const { instance } = await WebAssembly.instantiate(wasmBytes, { env: { memory } });
  const api = instance.exports;

  console.log("[test] exports:", Object.keys(api).sort().join(", "));

  if (typeof api.set_area_content !== "function") {
    throw new Error("missing export: set_area_content");
  }
  if (typeof api.set_handle_content !== "function") {
    throw new Error("missing export: set_handle_content");
  }
  if (typeof api.resize_screen !== "function") {
    throw new Error("missing export: resize_screen");
  }

  assertEq(api.move_handle(0, 10, 10), ERR.NOT_INITIALIZED, "move_handle before init must fail");
  assertEq(api.move_corner(0, 0, 10, 10), ERR.NOT_INITIALIZED, "move_corner before init must fail");

  assertEq(api.init_screen(0, 300, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.INVALID_ARG, "init_screen width=0 must fail");
  assertEq(api.init_screen(800, 600, 0, MIN_PANEL_SIZE), ERR.INVALID_ARG, "init_screen handle_size=0 must fail");
  assertEq(api.init_screen(800, 600, HANDLE_SIZE, 0), ERR.INVALID_ARG, "init_screen min_panel_size=0 must fail");
  assertEq(api.init_screen(800, 600, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "init_screen must succeed");

  const dataPtr = api.get_info_ptr();
  if (!dataPtr) {
    throw new Error("get_info_ptr returned null");
  }

  const header = new Int32Array(memory.buffer, dataPtr, ABI.HEADER_I32);
  const initialized = header[0];
  const screenW = header[1];
  const screenH = header[2];
  const maxPanels = header[3];
  const maxHandles = header[4];
  const areaCount = header[5];
  const handleCount = header[6];
  const generationAfterInit = header[8];
  const handleHalfSize = header[13];
  const minPanelSize = header[14];

  assertEq(initialized, 1, "initialized flag");
  assertEq(screenW, 800, "screen width");
  assertEq(screenH, 600, "screen height");
  assertEq(maxPanels, 16, "max panels constant");
  assertEq(maxHandles, ABI.MAX_PANELS - 1, "max handles constant");
  assertEq(areaCount, 1, "area count after init");
  assertEq(handleCount, 0, "handle count after init");
  assertEq(handleHalfSize, 4, "handle half size after init");
  assertEq(minPanelSize, MIN_PANEL_SIZE, "min panel size after init");

  const area0Start = ABI.HEADER_I32;
  const area0 = new Int32Array(memory.buffer, dataPtr + area0Start * 4, ABI.AREA_I32);
  assertEq(area0[0], 0, "area0 x0");
  assertEq(area0[1], 0, "area0 y0");
  assertEq(area0[2], 800, "area0 x1");
  assertEq(area0[3], 600, "area0 y1");
  assertEq(area0[4], 0, "area0 content defaults to 0");

  const handleBaseI32 = ABI.HEADER_I32 + ABI.MAX_PANELS * ABI.AREA_I32;
  const handle0 = new Int32Array(memory.buffer, dataPtr + handleBaseI32 * 4, ABI.HANDLE_I32);

  assertEq(api.move_handle(0, 10, 10), ERR.INVALID_HANDLE, "no handles exist before first split");
  assertEq(api.move_handle(999, 10, 10), ERR.INVALID_HANDLE, "invalid handle id");
  assertEq(header[8], generationAfterInit, "generation unchanged after invalid handle id");

  assertEq(api.set_area_content(0, 42), ERR.OK, "set_area_content succeeds");
  assertEq(area0[4], 42, "area0 content updated");

  assertEq(api.move_corner(0, 2, 700, 500), ERR.OK, "inside-corner move splits area");
  assertEq(header[5], 2, "area count after first split");
  assertEq(header[6], 1, "handle count after first split");
  assertEq(area0[2], 700, "area0 x1 after split");
  assertEq(area0[3], 600, "area0 y1 after split");

  const area1Start = ABI.HEADER_I32 + ABI.AREA_I32;
  const area1 = new Int32Array(memory.buffer, dataPtr + area1Start * 4, ABI.AREA_I32);
  assertEq(area1[0], 700, "area1 x0 after split");
  assertEq(area1[1], 0, "area1 y0 after split");
  assertEq(area1[2], 800, "area1 x1 after split");
  assertEq(area1[3], 600, "area1 y1 after split");
  assertEq(area1[4], 42, "new area inherits content id");

  assertEq(api.move_handle(0, -1, 10), ERR.OUT_OF_BOUNDS, "negative x is invalid");
  assertEq(api.move_handle(0, 400, 300), ERR.OK, "valid handle move after split");
  assertEq(area0[2], 400, "area0 x1 resized by handle move");
  assertEq(area1[0], 400, "area1 x0 resized by handle move");
  assertEq(handle0[0], 396, "handle0 x0 after move");
  assertEq(handle0[1], 0, "handle0 y0 spans full split range");
  assertEq(handle0[2], 404, "handle0 x1 after move");
  assertEq(handle0[3], 600, "handle0 y1 spans full split range");

  assertEq(api.move_corner(123, 0, 50, 50), ERR.INVALID_AREA, "invalid area id");
  assertEq(api.move_corner(0, 9, 50, 50), ERR.INVALID_CORNER, "invalid corner index");
  assertEq(api.move_corner(0, 0, 800, 600), ERR.OUT_OF_BOUNDS, "outside-target move returns out-of-bounds");
  assertEq(api.move_corner(0, 1, 720, 200), ERR.NOT_IMPLEMENTED, "cross-area corner move triggers merge-not-implemented");
  assertEq(api.set_area_content(9, 42), ERR.INVALID_AREA, "set_area_content invalid area");

  assertEq(api.move_corner(0, 2, 200, 300), ERR.OK, "second split inside area succeeds");
  assertEq(api.move_handle(0, 450, 200), ERR.OK, "original handle still movable after second split");

  assertEq(api.set_handle_content(0, 777), ERR.OK, "set_handle_content succeeds");
  assertEq(handle0[4], 777, "handle0 content updated");
  assertEq(api.set_handle_content(99, 1), ERR.INVALID_HANDLE, "set_handle_content invalid handle");

  const preResizeArea0X1 = area0[2];
  const preResizeArea0Y1 = area0[3];
  const preResizeArea1X0 = area1[0];
  const preResizeArea1X1 = area1[2];
  assertEq(api.resize_screen(1200, 900, HANDLE_SIZE), ERR.OK, "resize_screen succeeds");
  assertEq(header[1], 1200, "screen width after resize");
  assertEq(header[2], 900, "screen height after resize");
  assertEq(area0[2], Math.trunc(preResizeArea0X1 * 1200 / 800), "area0 x1 scales on resize");
  assertEq(area0[3], Math.trunc(preResizeArea0Y1 * 900 / 600), "area0 y1 scales on resize");
  assertEq(area1[0], Math.trunc(preResizeArea1X0 * 1200 / 800), "area1 x0 scales on resize");
  assertEq(area1[2], Math.trunc(preResizeArea1X1 * 1200 / 800), "area1 x1 scales on resize");

  console.log("[test] basic checks passed");

  /* ========================================================
   * Regression: 3-column layout with horizontal splits
   * ========================================================
   *
   * Layout:
   *   col0 (0..300) | col1 (300..600) | col2 (600..900)
   * Then split col0 horizontally at y=250, col1 at y=350.
   *
   * Expected: h0 (vertical, x=300) spans full 0..600 height
   *           h1 (vertical, x=600) spans full 0..600 height
   *           h2 (horizontal in col0) spans 0..300
   *           h3 (horizontal in col1) spans 300..600
   *           + and T-junction handles stay independent
   */
  assertEq(api.init_screen(900, 600, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "3col: init_screen");

  function areaAtIndex(idx) {
    const base = ABI.HEADER_I32 + idx * ABI.AREA_I32;
    return new Int32Array(memory.buffer, dataPtr + base * 4, ABI.AREA_I32);
  }

  function handleAtIndex(idx) {
    const base = ABI.HEADER_I32 + ABI.MAX_PANELS * ABI.AREA_I32 + idx * ABI.HANDLE_I32;
    return new Int32Array(memory.buffer, dataPtr + base * 4, ABI.HANDLE_I32);
  }

  // Split into 2 columns: area0=[0,0,300,600] area1=[300,0,900,600]
  assertEq(api.move_corner(0, 2, 300, 400), ERR.OK, "3col: first vertical split");
  assertEq(header[5], 2, "3col: 2 areas after first split");

  // Split into 3 columns: area1=[300,0,600,600] area2=[600,0,900,600]
  assertEq(api.move_corner(1, 2, 600, 400), ERR.OK, "3col: second vertical split");
  assertEq(header[5], 3, "3col: 3 areas after second split");
  assertEq(header[6], 2, "3col: 2 handles (h0, h1) after 3 columns");

  const h0 = handleAtIndex(0);
  const h1 = handleAtIndex(1);

  // h0 should be at x=300, full height
  assertEq(h0[1], 0, "3col: h0 y0 = 0 (full height)");
  assertEq(h0[3], 600, "3col: h0 y1 = 600 (full height)");

  // h1 should be at x=600, full height
  assertEq(h1[1], 0, "3col: h1 y0 = 0 (full height)");
  assertEq(h1[3], 600, "3col: h1 y1 = 600 (full height)");

  // Split col0 horizontally at y=250
  assertEq(api.move_corner(0, 2, 150, 250), ERR.OK, "3col: split col0 horizontally");
  assertEq(header[5], 4, "3col: 4 areas after col0 h-split");
  assertEq(header[6], 3, "3col: 3 handles after col0 h-split");

  // Split col1 horizontally at y=350
  assertEq(api.move_corner(1, 2, 450, 350), ERR.OK, "3col: split col1 horizontally");
  assertEq(header[5], 5, "3col: 5 areas after col1 h-split");
  assertEq(header[6], 4, "3col: 4 handles after col1 h-split");

  // h0 must STILL span full height (the key bug)
  assertEq(h0[1], 0, "3col: h0 y0 still 0 after h-splits");
  assertEq(h0[3], 600, "3col: h0 y1 still 600 after h-splits");

  // h1 must STILL span full height
  assertEq(h1[1], 0, "3col: h1 y0 still 0 after h-splits");
  assertEq(h1[3], 600, "3col: h1 y1 still 600 after h-splits");

  // h2 should be horizontal, spanning only col0 width
  const h2 = handleAtIndex(2);
  assertEq(h2[0], 0, "3col: h2 x0 = 0 (col0 left edge)");
  assertEq(h2[2], 300, "3col: h2 x1 = 300 (col0 right edge)");

  // h3 should be horizontal, spanning only col1 width
  const h3 = handleAtIndex(3);
  assertEq(h3[0], 300, "3col: h3 x0 = 300 (col1 left edge)");
  assertEq(h3[2], 600, "3col: h3 x1 = 600 (col1 right edge)");

  // Move h0 (full-height vertical) - should work
  assertEq(api.move_handle(0, 350, 300), ERR.OK, "3col: move h0 to x=350");
  // All areas on left of h0 should have x1=350, all on right x0=350
  const a0 = areaAtIndex(0);
  const a3 = areaAtIndex(3);
  assertEq(a0[2], 350, "3col: area0 x1 after h0 move");
  assertEq(a3[2], 350, "3col: area3 x1 after h0 move");

  // Move h2 (horizontal in col0) - should still work independently
  assertEq(api.move_handle(2, 175, 300), ERR.OK, "3col: move h2 to y=300");

  // Move h3 (horizontal in col1) - should still work independently
  assertEq(api.move_handle(3, 475, 400), ERR.OK, "3col: move h3 to y=400");

  console.log("[test] 3-column regression passed");

  /* ========================================================
   * Regression: sticky handles (two horizontal handles meeting
   * at a vertical split should NOT merge when moved to same y)
   * ========================================================
   * Repro: init 400x400, split vertical at x=185, split left at y=116,
   * split right at y=164. Then move left h-handle toward y=164.
   * Handles must stay independent.
   */
  assertEq(api.init_screen(400, 400, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "sticky: init_screen");
  assertEq(api.move_corner(0, 1, 185, 40), ERR.OK, "sticky: vertical split");
  assertEq(api.move_corner(0, 0, 7, 116), ERR.OK, "sticky: left h-split");
  assertEq(api.move_corner(1, 1, 392, 164), ERR.OK, "sticky: right h-split");

  // h0 = vertical at x=185 (full height)
  // h1 = horizontal in left col (x: 0..185)
  // h2 = horizontal in right col (x: 185..400)
  const sh0 = handleAtIndex(0);
  const sh1 = handleAtIndex(1);
  const sh2 = handleAtIndex(2);

  // Verify h1 spans only left column
  assertEq(sh1[0], 0, "sticky: h1 x0 = 0 (left col)");
  assertEq(sh1[2], 185, "sticky: h1 x1 = 185 (left col)");

  // Verify h2 spans only right column
  assertEq(sh2[0], 185, "sticky: h2 x0 = 185 (right col)");
  assertEq(sh2[2], 400, "sticky: h2 x1 = 400 (right col)");

  // Move h1 down to same y as h2 (y=164)
  assertEq(api.move_handle(1, 92, 164), ERR.OK, "sticky: move h1 to y=164");

  // h1 must STILL span only left column
  assertEq(sh1[0], 0, "sticky: h1 x0 still 0 after move to same y");
  assertEq(sh1[2], 185, "sticky: h1 x1 still 185 after move to same y");

  // h2 must STILL span only right column
  assertEq(sh2[0], 185, "sticky: h2 x0 still 185 after move to same y");
  assertEq(sh2[2], 400, "sticky: h2 x1 still 400 after move to same y");

  // Both handles should still be independently movable
  assertEq(api.move_handle(1, 92, 200), ERR.OK, "sticky: h1 still movable independently");
  assertEq(api.move_handle(2, 292, 164), ERR.OK, "sticky: h2 still movable independently");

  console.log("[test] sticky-handle regression passed");

  /* ========================================================
   * Regression: aligned horizontal handles shrink vertical handle
   * ========================================================
   * Repro: init 400x400, vertical split at x=182, horizontal split
   * left col at y=241, horizontal split right col at y=172.
   * Then move one horizontal handle to align with the other.
   * The vertical handle must STILL span full height 0..400.
   *
   * Layout before alignment:
   *   area0=[0,0,182,241]     area1=[182,0,400,172]
   *   area2=[0,241,182,400]   area3=[182,172,400,400]
   *
   * After aligning h2 to y=241 (or h1 to y=172), the vertical
   * handle h0 at x=182 must still go 0..400.
   */
  assertEq(api.init_screen(400, 400, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "aligned: init_screen");

  // Vertical split at x=182
  assertEq(api.move_corner(0, 0, 182, 114), ERR.OK, "aligned: vertical split");
  assertEq(header[5], 2, "aligned: 2 areas");
  // area0=[0,0,182,400], area1=[182,0,400,400]

  // Horizontal split left col at y=241
  assertEq(api.move_corner(0, 0, 50, 241), ERR.OK, "aligned: h-split left col");
  assertEq(header[5], 3, "aligned: 3 areas");
  // area0=[0,0,182,241], area2=[0,241,182,400]

  // Horizontal split right col at y=172
  assertEq(api.move_corner(1, 1, 281, 172), ERR.OK, "aligned: h-split right col");
  assertEq(header[5], 4, "aligned: 4 areas");
  // area1=[182,0,400,172], area3=[182,172,400,400]

  const ah0 = handleAtIndex(0); // vertical at x=182
  const ah1 = handleAtIndex(1); // horizontal at y=241 (left col)
  const ah2 = handleAtIndex(2); // horizontal at y=172 (right col)

  // Verify vertical handle spans full height before alignment
  assertEq(ah0[1], 0, "aligned: h0 y0=0 before alignment");
  assertEq(ah0[3], 400, "aligned: h0 y1=400 before alignment");

  // Move h1 (left horizontal) to align with h2 at y=172
  assertEq(api.move_handle(1, 91, 172), ERR.OK, "aligned: move h1 to y=172");

  // Vertical handle h0 MUST still span full height
  assertEq(ah0[1], 0, "aligned: h0 y0 still 0 after alignment");
  assertEq(ah0[3], 400, "aligned: h0 y1 still 400 after alignment");

  // h1 and h2 must stay in their respective columns
  assertEq(ah1[0], 0, "aligned: h1 x0=0 (left col only)");
  assertEq(ah1[2], 182, "aligned: h1 x1=182 (left col only)");
  assertEq(ah2[0], 182, "aligned: h2 x0=182 (right col only)");
  assertEq(ah2[2], 400, "aligned: h2 x1=400 (right col only)");

  // Also test alignment in the other direction: move h2 to y=172 back
  // Reset with same layout
  assertEq(api.init_screen(400, 400, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "aligned2: init_screen");
  assertEq(api.move_corner(0, 0, 182, 114), ERR.OK, "aligned2: vertical split");
  assertEq(api.move_corner(0, 0, 50, 241), ERR.OK, "aligned2: h-split left col");
  assertEq(api.move_corner(1, 1, 281, 172), ERR.OK, "aligned2: h-split right col");

  const bh0 = handleAtIndex(0);
  const bh1 = handleAtIndex(1);
  const bh2 = handleAtIndex(2);

  // Move h2 (right horizontal) to align with h1 at y=241
  assertEq(api.move_handle(2, 291, 241), ERR.OK, "aligned2: move h2 to y=241");

  // Vertical handle MUST still span full height
  assertEq(bh0[1], 0, "aligned2: h0 y0 still 0 after alignment");
  assertEq(bh0[3], 400, "aligned2: h0 y1 still 400 after alignment");

  // Handles must stay in their respective columns
  assertEq(bh1[0], 0, "aligned2: h1 x0=0 (left col only)");
  assertEq(bh1[2], 182, "aligned2: h1 x1=182 (left col only)");
  assertEq(bh2[0], 182, "aligned2: h2 x0=182 (right col only)");
  assertEq(bh2[2], 400, "aligned2: h2 x1=400 (right col only)");

  // And both handles must be independently movable after alignment
  assertEq(api.move_handle(1, 91, 200), ERR.OK, "aligned2: h1 movable after align");
  assertEq(api.move_handle(2, 291, 300), ERR.OK, "aligned2: h2 movable after align");

  // Vertical handle STILL full height
  assertEq(bh0[1], 0, "aligned2: h0 y0 still 0 after post-align moves");
  assertEq(bh0[3], 400, "aligned2: h0 y1 still 400 after post-align moves");

  console.log("[test] aligned-handles regression passed");

  /* ========================================================
   * Regression: moving a horizontal handle must update the
   * y-spans of perpendicular (vertical) handles that share
   * the same boundary row.
   * ========================================================
   *
   * Layout after 4 ops on 400x400:
   *
   *   move_corner(0, 0, 127, 128)  → h-split at y=128 (full width)
   *   move_corner(0, 1, 232,   9)  → v-split at x=232 (top row only, y=0..128)
   *   move_corner(1, 3, 130, 289)  → v-split at x=130 (bottom row only, y=128..400)
   *
   *   area0=[0,0,232,128]   area2=[232,0,400,128]
   *   area1=[0,128,130,400] area3=[130,128,400,400]
   *
   *   h0 = horizontal at y=128, x=0..400
   *   h1 = vertical   at x=232, y=0..128
   *   h2 = vertical   at x=130, y=128..400
   *
   * Moving h0 up 10px (y=118) must:
   *   - shrink h1's y-span: 0..128 → 0..118  (top boundary moves up)
   *   - grow   h2's y-span: 128..400 → 118..400 (top boundary moves up)
   */
  assertEq(api.init_screen(400, 400, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "hspan: init");
  assertEq(api.move_corner(0, 0, 127, 128), ERR.OK, "hspan: h-split at y=128");
  assertEq(api.move_corner(0, 1, 232,   9), ERR.OK, "hspan: v-split top at x=232");
  assertEq(api.move_corner(1, 3, 130, 289), ERR.OK, "hspan: v-split bottom at x=130");

  assertEq(header[5], 4, "hspan: 4 areas");
  assertEq(header[6], 3, "hspan: 3 handles");

  const rh0 = handleAtIndex(0); // horizontal at y=128
  const rh1 = handleAtIndex(1); // vertical   at x=232, y=0..128
  const rh2 = handleAtIndex(2); // vertical   at x=130, y=128..400

  // Verify initial spans
  assertEq(rh1[1],   0, "hspan: h1 y0 initially 0");
  assertEq(rh1[3], 128, "hspan: h1 y1 initially 128");
  assertEq(rh2[1], 128, "hspan: h2 y0 initially 128");
  assertEq(rh2[3], 400, "hspan: h2 y1 initially 400");

  // Move h0 up 10px
  assertEq(api.move_handle(0, 200, 118), ERR.OK, "hspan: move h0 to y=118");

  // h1 must shrink (top row got shorter)
  assertEq(rh1[1],   0, "hspan: h1 y0 still 0 after h0 move");
  assertEq(rh1[3], 118, "hspan: h1 y1 updated to 118 after h0 move");

  // h2 must grow (bottom row got taller)
  assertEq(rh2[1], 118, "hspan: h2 y0 updated to 118 after h0 move");
  assertEq(rh2[3], 400, "hspan: h2 y1 still 400 after h0 move");

  // Move h0 back down 20px (y=138) — spans must follow
  assertEq(api.move_handle(0, 200, 138), ERR.OK, "hspan: move h0 to y=138");
  assertEq(rh1[3], 138, "hspan: h1 y1 updated to 138");
  assertEq(rh2[1], 138, "hspan: h2 y0 updated to 138");

  console.log("[test] handle-span-update regression passed");

  /* ========================================================
   * Regression: moving a vertical handle must update the
   * x-spans of perpendicular (horizontal) handles whose
   * column wall was that vertical boundary.
   * ========================================================
   *
   * Layout after 3 ops on 400x400:
   *
   *   move_corner(0, 0, 181,  65) → v-split at x=181 (full height)
   *   move_corner(1, 1, 277, 258) → h-split at y=258 (right col, x=181..400)
   *   move_corner(0, 0,  84, 192) → h-split at y=192 (left col,  x=0..181)
   *
   *   area0=[0,0,181,192]   area1=[181,0,400,258]
   *   area3=[0,192,181,400] area2=[181,258,400,400]
   *
   *   h0 = vertical   at x=181, y=0..400
   *   h1 = horizontal at y=258, x=181..400  (col_x0=181, col_x1=400)
   *   h2 = horizontal at y=192, x=0..181    (col_x0=0,   col_x1=181)
   *
   * Moving h0 left to x=129 must:
   *   - shrink h1's x-span: 181..400 → 129..400  (left wall moved left)
   *   - shrink h2's x-span: 0..181   → 0..129    (right wall moved left)
   */
  assertEq(api.init_screen(400, 400, HANDLE_SIZE, MIN_PANEL_SIZE), ERR.OK, "vspan: init");
  assertEq(api.move_corner(0, 0, 181,  65), ERR.OK, "vspan: v-split at x=181");
  assertEq(api.move_corner(1, 1, 277, 258), ERR.OK, "vspan: h-split right col at y=258");
  assertEq(api.move_corner(0, 0,  84, 192), ERR.OK, "vspan: h-split left col at y=192");

  assertEq(header[5], 4, "vspan: 4 areas");
  assertEq(header[6], 3, "vspan: 3 handles");

  const vh0 = handleAtIndex(0); // vertical   at x=181
  const vh1 = handleAtIndex(1); // horizontal at y=258, x=181..400
  const vh2 = handleAtIndex(2); // horizontal at y=192, x=0..181

  // Verify initial spans
  assertEq(vh1[0], 181, "vspan: h1 x0 initially 181");
  assertEq(vh1[2], 400, "vspan: h1 x1 initially 400");
  assertEq(vh2[0],   0, "vspan: h2 x0 initially 0");
  assertEq(vh2[2], 181, "vspan: h2 x1 initially 181");

  // Move h0 left to x=129
  assertEq(api.move_handle(0, 129, 112), ERR.OK, "vspan: move h0 to x=129");

  // h1 left wall must follow (col_x0 was 181, now 129)
  assertEq(vh1[0], 129, "vspan: h1 x0 updated to 129 after h0 move");
  assertEq(vh1[2], 400, "vspan: h1 x1 still 400");

  // h2 right wall must follow (col_x1 was 181, now 129)
  assertEq(vh2[0],   0, "vspan: h2 x0 still 0");
  assertEq(vh2[2], 129, "vspan: h2 x1 updated to 129 after h0 move");

  // Move h0 right to x=220 — spans must follow again
  assertEq(api.move_handle(0, 220, 112), ERR.OK, "vspan: move h0 to x=220");
  assertEq(vh1[0], 220, "vspan: h1 x0 updated to 220");
  assertEq(vh2[2], 220, "vspan: h2 x1 updated to 220");

  // h1 and h2 must still be independently movable
  assertEq(api.move_handle(1, 310, 300), ERR.OK, "vspan: h1 still movable");
  assertEq(api.move_handle(2, 110, 250), ERR.OK, "vspan: h2 still movable");

  console.log("[test] vertical-handle-span-update regression passed");
  console.log("[test] all checks passed");
}

run().catch((err) => {
  console.error("[test] failed:", err.message);
  process.exit(1);
});
