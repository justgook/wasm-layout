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

  assertEq(api.set_handle_content(0, 777), ERR.OK, "set_handle_content succeeds");
  assertEq(handle0[4], 777, "handle0 content updated");
  assertEq(api.set_handle_content(99, 1), ERR.INVALID_HANDLE, "set_handle_content invalid handle");

  assertEq(api.resize_screen(1200, 900), ERR.OK, "resize_screen succeeds");
  assertEq(header[1], 1200, "screen width after resize");
  assertEq(header[2], 900, "screen height after resize");
  assertEq(area0[2], 600, "area0 x1 scales on resize");
  assertEq(area0[3], 900, "area0 y1 scales on resize");
  assertEq(area1[0], 600, "area1 x0 scales on resize");
  assertEq(area1[2], 1200, "area1 x1 scales on resize");

  console.log("[test] all checks passed");
}

run().catch((err) => {
  console.error("[test] failed:", err.message);
  process.exit(1);
});
