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

  assertEq(api.move_handle(0, 10, 10), ERR.NOT_INITIALIZED, "move_handle before init must fail");
  assertEq(api.move_corner(0, 0, 10, 10), ERR.NOT_INITIALIZED, "move_corner before init must fail");

  assertEq(api.init_screen(0, 300), ERR.INVALID_ARG, "init_screen width=0 must fail");
  assertEq(api.init_screen(800, 600), ERR.OK, "init_screen must succeed");

  const dataPtr = api.get_data_ptr();
  if (!dataPtr) {
    throw new Error("get_data_ptr returned null");
  }

  const header = new Int32Array(memory.buffer, dataPtr, 12);
  const initialized = header[0];
  const screenW = header[1];
  const screenH = header[2];
  const maxPanels = header[3];
  const areaCount = header[4];
  const handleCount = header[5];

  assertEq(initialized, 1, "initialized flag");
  assertEq(screenW, 800, "screen width");
  assertEq(screenH, 600, "screen height");
  assertEq(maxPanels, 16, "max panels constant");
  assertEq(areaCount, 1, "area count after init");
  assertEq(handleCount, 4, "handle count after init");

  assertEq(api.move_handle(999, 10, 10), ERR.INVALID_HANDLE, "invalid handle id");
  assertEq(api.move_handle(1, -1, 10), ERR.OUT_OF_BOUNDS, "negative x is invalid");
  assertEq(api.move_handle(1, 400, 300), ERR.OK, "valid handle move");

  assertEq(api.move_corner(123, 0, 50, 50), ERR.INVALID_AREA, "invalid area id");
  assertEq(api.move_corner(0, 9, 50, 50), ERR.INVALID_CORNER, "invalid corner index");
  assertEq(api.move_corner(0, 0, 790, 590), ERR.MIN_SIZE, "corner move violating min size");
  assertEq(api.move_corner(0, 2, 700, 500), ERR.OK, "valid corner move");

  console.log("[test] all checks passed");
}

run().catch((err) => {
  console.error("[test] failed:", err.message);
  process.exit(1);
});
