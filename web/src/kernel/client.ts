import type {
  BenchResult,
  DiffChange,
  Frame,
  PageInfo,
  StateInfo,
  Stats,
  VerifyResult,
  World,
} from "./types";
import type { ForkInfo, PageReport, SpecResult, VmState } from "../play/types";
import type { CallView, FileChange, FileContent, FileEntry, RecordingView, RunDiffView } from "../viewer/types";

type Pending = { resolve: (v: any) => void; reject: (e: Error) => void };

/** Typed RPC facade over the kernel Web Worker. */
export class KernelClient {
  private worker: Worker;
  private seq = 0;
  private pending = new Map<number, Pending>();
  /** Round-trip time of the last call (worker compute only), for the HUD. */
  lastMs = 0;

  constructor() {
    this.worker = new Worker(new URL("./worker.ts", import.meta.url), { type: "module" });
    this.worker.onmessage = (e: MessageEvent<{ id: number; result?: unknown; error?: string; ms?: number }>) => {
      const p = this.pending.get(e.data.id);
      if (!p) return;
      this.pending.delete(e.data.id);
      if (e.data.ms !== undefined) this.lastMs = e.data.ms;
      if (e.data.error) p.reject(new Error(e.data.error));
      else p.resolve(e.data.result);
    };
  }

  private call<T>(method: string, ...args: unknown[]): Promise<T> {
    const id = ++this.seq;
    return new Promise<T>((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.worker.postMessage({ id, method, args });
    });
  }

  forkCost = () => this.call<number>("forkCost");
  init = (seed?: number) => this.call<number>("init", seed);
  world = () => this.call<World>("world");
  stats = () => this.call<Stats>("stats");
  step = (tid: number, n = 1) => this.call<{ executed: number }>("step", tid, n);
  fork = (tid: number, g: number) => this.call<{ id: number; fork_us: number }>("fork", tid, g);
  inject = (tid: number, key: string, text: string) => this.call<object>("inject", tid, key, text);
  kill = (tid: number, actor: number) => this.call<object>("kill", tid, actor);
  speculate = (tid: number, g: number, n: number) => this.call<{ ranked: number[] }>("speculate", tid, g, n);
  state = (tid: number, g: number) => this.call<StateInfo>("state", tid, g);
  file = (tid: number, g: number, path: string) => this.call<{ text: string | null }>("file", tid, g, path);
  pages = (tid: number, g: number, path: string, btid: number, bg: number) =>
    this.call<{ size: number; pages: PageInfo[] }>("pages", tid, g, path, btid, bg);
  diff = (t1: number, g1: number, t2: number, g2: number) => this.call<{ changes: DiffChange[] }>("diff", t1, g1, t2, g2);
  frames = (tid: number, g: number) => this.call<{ frames?: Frame[]; inherited?: boolean }>("frames", tid, g);
  verify = (tid: number) => this.call<VerifyResult>("verify", tid);
  tamper = (tid: number, g: number) => this.call<number>("tamper", tid, g);
  corrupt = (tid: number, g: number) => this.call<number>("corrupt", tid, g);
  gc = () => this.call<{ freed: number; ms: number }>("gc");
  bench = (quick: boolean) => this.call<BenchResult>("bench", quick);
  exportLog = (tid: number) => this.call<Uint8Array<ArrayBuffer>>("exportLog", tid);
  // Recordings (.khn) for the viewer.
  recLoad = (bytes: Uint8Array) => this.call<RecordingView & { handle: number }>("recLoad", bytes);
  recCall = (h: number, i: number) => this.call<CallView>("recCall", h, i);
  recFiles = (h: number, root: string) => this.call<FileEntry[]>("recFiles", h, root);
  recFile = (h: number, root: string, path: string) => this.call<FileContent>("recFile", h, root, path);
  recChanges = (h: number, from: string, to: string) => this.call<FileChange[]>("recChanges", h, from, to);
  recDiff = (a: number, b: number) => this.call<RunDiffView>("recDiff", a, b);
  recClose = (h: number) => this.call<void>("recClose", h);
  // Lune (Play): a forkable deterministic VM.
  luneNew = (src: string, gas: number) => this.call<VmState | { error: string; line: number }>("luneNew", src, gas);
  luneRun = (id: number, steps: number) => this.call<VmState>("luneRun", id, steps);
  luneState = (id: number) => this.call<VmState>("luneState", id);
  luneFork = (id: number) => this.call<ForkInfo>("luneFork", id);
  luneSet = (id: number, name: string, v: number) => this.call<VmState>("luneSet", id, name, v);
  luneSpeculate = (id: number, v: string, from: number, n: number, budget: number) =>
    this.call<SpecResult>("luneSpeculate", id, v, from, n, budget);
  lunePages = (id: number) => this.call<PageReport>("lunePages", id);
  luneFree = (id: number) => this.call<void>("luneFree", id);
  luneGc = () => this.call<{ freed: number; ms: number; store_pages: number; store_bytes: number }>("luneGc");
  importLog = (bytes: Uint8Array) => this.call<{ id: number; error: string }>("importLog", bytes);
}

let shared: KernelClient | null = null;
/** One kernel worker for the whole app, created on first use (the landing page never pays for it). */
export function kernel(): KernelClient {
  if (!shared) shared = new KernelClient();
  return shared;
}
