// Shapes of the JSON documents produced by platform/web/bindings.cpp.

export interface StepInfo {
  g: number;
  actor: number;
  kind: "genesis" | "dispatch" | "edit" | "fault";
  tag: string;
  label: string;
  vtime: number;
  digest: string;
  tokens: number;
  saved: number;
  wall_us: number;
  lsn0: number;
  lsn1: number;
  live: boolean;
}

export interface SpanInfo {
  a: number;
  k: "step" | "llm" | "tool";
  c: boolean;
  g: number;
  t0: number;
  t1: number;
  n: string;
}

export interface TimelineInfo {
  id: number;
  parent: number;
  base: number;
  name: string;
  sample: string;
  pruned: boolean;
  status: "running" | "done" | "failed";
  head: number;
  tokens: number;
  tokens_saved: number;
  llm_calls: number;
  cache_hits: number;
  log_frames: number;
  log_bytes: number;
  steps: StepInfo[];
  spans: SpanInfo[];
}

export interface Stats {
  store: {
    branches: number;
    entries: number;
    blobs: number;
    pages: number;
    files: number;
    bytes: number;
    pages_allocated: number;
    page_bytes_copied: number;
  };
  metrics: {
    last_fork_us: number;
    steps_executed: number;
    avg_step_us: number;
    last_verify_ms: number;
    last_verify_steps: number;
    last_gc_ms: number;
    last_gc_freed: number;
  };
  model: string;
  cache_entries: number;
  heap_bytes: number;
}

export interface World extends Stats {
  timelines: TimelineInfo[];
}

export interface StateEntry {
  key: string;
  digest: string;
  type: "file" | "blob";
  size: number;
  pages?: number;
  preview?: string;
}

export interface PendingEvent {
  vtime: number;
  seq: number;
  to: number;
  from: number;
  kind: number;
  topic: string;
  body: string;
}

export interface StateInfo {
  root: string;
  entries: StateEntry[];
  pending: PendingEvent[];
}

export interface PageInfo {
  i: number;
  s: "shared" | "private" | "new";
  ref: number;
  d: string;
}

export interface Frame {
  lsn: number;
  type: string;
  actor: number;
  vtime: number;
  bytes: number;
  crc: string;
  crc_ok: boolean;
  hash?: string;
  role?: string;
  sample?: string;
  prompt?: string;
  cached?: boolean;
  latency_us?: number;
  prompt_tokens?: number;
  completion_tokens?: number;
  text?: string;
  step?: number;
  state?: string;
  chain?: string;
  kind?: number;
  parent?: number;
  scenario?: string;
}

export interface DiffChange {
  key: string;
  kind: "added" | "removed" | "modified";
  before?: string;
  after?: string;
}

export interface VerifyResult {
  ok: boolean;
  steps: number;
  ms: number;
  step: number;
  reason: string;
  diff: string[];
}

export interface BenchForkRow {
  pages: number;
  mib: number;
  khonsu_fork_ns: number;
  khonsu_first_write_ns: number;
  v1_fork_ns: number;
  v1_first_write_ns: number;
  deep_fork_ns?: number;
  deep_first_write_ns?: number;
}

export interface BenchResult {
  target: string;
  fork: BenchForkRow[];
  kernel: { fork_ns: number; timelines_created: number; verify_steps: number; verify_us_per_step: number; avg_step_us: number };
  log: { frames: number; payload_bytes: number; append_mframes_per_s: number; append_ns_per_frame: number; crc32c_gib_per_s: number };
}

export const ACTORS = ["user", "planner", "researcher", "coder", "tester", "reviewer", "supervisor"] as const;
