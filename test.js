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
  HEADER_I32: 13,
  AREA_I32: 5,
  HANDLE_I32: 5,
  MAX_PANELS: 16,
};

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

  assertEq(api.init_screen(0, 300), ERR.INVALID_ARG, "init_screen width=0 must fail");
  assertEq(api.init_screen(800, 600), ERR.OK, "init_screen must succeed");

  const dataPtr = api.get_data_ptr();
  if (!dataPtr) {
    throw new Error("get_data_ptr returned null");
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

  assertEq(initialized, 1, "initialized flag");
  assertEq(screenW, 800, "screen width");
  assertEq(screenH, 600, "screen height");
  assertEq(maxPanels, 16, "max panels constant");
  assertEq(maxHandles, 64, "max handles constant");
  assertEq(areaCount, 1, "area count after init");
  assertEq(handleCount, 0, "handle count after init");

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
  assertEq(api.resize_screen(1200, 900), ERR.OK, "resize_screen succeeds");
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
  assertEq(api.init_screen(900, 600), ERR.OK, "3col: init_screen");

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
  console.log("[test] all checks passed");
}

run().catch((err) => {
  console.error("[test] failed:", err.message);
  process.exit(1);
});
