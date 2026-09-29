/// <reference lib="webworker" />
// Kernel worker: owns the WebAssembly instance so the UI thread never blocks on
// kernel work (speculative search, verification, benchmarks).

type Ccall = (name: string, ret: string | null, types: string[], args: unknown[]) => unknown;

interface KhonsuModule {
  ccall: Ccall;
  HEAPU8: Uint8Array;
  _malloc(n: number): number;
  _free(p: number): void;
}

let mod: KhonsuModule | null = null;

async function load(): Promise<KhonsuModule> {
  if (mod) return mod;
  const url = new URL(`${import.meta.env.BASE_URL}wasm/khonsu.mjs`, self.location.origin).href;
  const factory = (await import(/* @vite-ignore */ url)).default as () => Promise<KhonsuModule>;
  mod = await factory();
  return mod;
}

const S = "string";
const N = "number";

function json(m: KhonsuModule, name: string, types: string[], args: unknown[]) {
  const out = JSON.parse(m.ccall(name, S, types, args) as string);
  if (out && typeof out === "object" && "error" in out && out.error && !("id" in out)) throw new Error(out.error);
  return out;
}

const methods: Record<string, (m: KhonsuModule, ...a: any[]) => unknown> = {
  init: (m, seed?: number) => {
    const s = seed ?? (m.ccall("kh_demo_seed", N, [], []) as number);
    return m.ccall("kh_init", N, [N], [s]);
  },
  forkCost: (m) => m.ccall("kh_fork_cost", N, [], []),
  world: (m) => json(m, "kh_world", [], []),
  stats: (m) => json(m, "kh_stats", [], []),
  step: (m, tid: number, n: number) => json(m, "kh_step", [N, N], [tid, n]),
  fork: (m, tid: number, g: number) => json(m, "kh_fork", [N, N], [tid, g]),
  inject: (m, tid: number, key: string, text: string) => json(m, "kh_inject", [N, S, S], [tid, key, text]),
  kill: (m, tid: number, actor: number) => json(m, "kh_kill", [N, N], [tid, actor]),
  speculate: (m, tid: number, g: number, n: number) => json(m, "kh_speculate", [N, N, N], [tid, g, n]),
  state: (m, tid: number, g: number) => json(m, "kh_state", [N, N], [tid, g]),
  file: (m, tid: number, g: number, path: string) => json(m, "kh_file", [N, N, S], [tid, g, path]),
  pages: (m, tid: number, g: number, path: string, btid: number, bg: number) =>
    json(m, "kh_pages", [N, N, S, N, N], [tid, g, path, btid, bg]),
  diff: (m, t1: number, g1: number, t2: number, g2: number) => json(m, "kh_diff", [N, N, N, N], [t1, g1, t2, g2]),
  frames: (m, tid: number, g: number) => json(m, "kh_frames", [N, N], [tid, g]),
  verify: (m, tid: number) => json(m, "kh_verify", [N], [tid]),
  tamper: (m, tid: number, g: number) => m.ccall("kh_tamper", N, [N, N], [tid, g]),
  corrupt: (m, tid: number, g: number) => m.ccall("kh_corrupt", N, [N, N], [tid, g]),
  gc: (m) => json(m, "kh_gc", [], []),
  bench: (m, quick: boolean) => json(m, "kh_bench", [N], [quick ? 1 : 0]),
  recLoad: (m, bytes: Uint8Array) => {
    const ptr = m._malloc(bytes.length);
    try {
      m.HEAPU8.set(bytes, ptr);
      return json(m, "kh_rec_load", [N, N], [ptr, bytes.length]);
    } finally {
      m._free(ptr);
    }
  },
  recCall: (m, h: number, i: number) => json(m, "kh_rec_call", [N, N], [h, i]),
  recFiles: (m, h: number, root: string) => json(m, "kh_rec_files", [N, S], [h, root]),
  recFile: (m, h: number, root: string, path: string) => json(m, "kh_rec_file", [N, S, S], [h, root, path]),
  recChanges: (m, h: number, from: string, to: string) => json(m, "kh_rec_changes", [N, S, S], [h, from, to]),
  recDiff: (m, a: number, b: number) => json(m, "kh_rec_diff", [N, N], [a, b]),
  recClose: (m, h: number) => m.ccall("kh_rec_close", null, [N], [h]),
  exportLog: (m, tid: number) => {
    const ptr = m.ccall("kh_export", N, [N], [tid]) as number;
    const len = m.ccall("kh_blob_len", N, [], []) as number;
    return m.HEAPU8.slice(ptr, ptr + len);
  },
  importLog: (m, bytes: Uint8Array) => {
    const ptr = m._malloc(bytes.length);
    try {
      m.HEAPU8.set(bytes, ptr);
      return JSON.parse(m.ccall("kh_import", S, [N, N], [ptr, bytes.length]) as string);
    } finally {
      m._free(ptr);
    }
  },
};

self.onmessage = async (e: MessageEvent<{ id: number; method: string; args: unknown[] }>) => {
  const { id, method, args } = e.data;
  try {
    const m = await load();
    const fn = methods[method];
    if (!fn) throw new Error(`unknown method ${method}`);
    const t0 = performance.now();
    const result = fn(m, ...args);
    const ms = performance.now() - t0;
    if (result instanceof Uint8Array) self.postMessage({ id, result, ms }, [result.buffer]);
    else self.postMessage({ id, result, ms });
  } catch (err) {
    self.postMessage({ id, error: err instanceof Error ? err.message : String(err) });
  }
};
