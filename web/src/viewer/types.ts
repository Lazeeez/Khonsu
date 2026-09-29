// Shapes produced by kernel/src/views.cpp (and by the OpenTelemetry importer).

export interface SnapshotInfo {
  when: 0 | 1 | 2; // run start, before a call, run end
  seq: number;
  root: string;
  files: number;
  bytes: number;
  new_objects: number;
  new_bytes: number;
  took_us: number;
}

export interface ExchangeInfo {
  seq: number;
  t_us: number;
  method: string;
  path: string;
  key: string;
  occurrence: number;
  tool: boolean;
  model: string;
  input: string;
  output: string;
  tool_calls: string;
  prompt_tokens: number;
  completion_tokens: number;
  snapshot?: string;
  status?: number;
  latency_us?: number;
  streamed?: boolean;
  from_recording?: boolean;
  workspace_changes: string[];
}

export interface RecordingView {
  format: string;
  mode: string;
  command: string;
  cwd: string;
  upstream: string;
  parent: string;
  workspace: string;
  tool_version: string;
  snapshots: number;
  objects: number;
  object_bytes: number;
  started_unix_ms: number;
  digest: string;
  chain_ok: boolean;
  complete: boolean;
  recovery: string;
  frames: number;
  exit_code?: number;
  duration_us?: number;
  snapshot_list: SnapshotInfo[];
  exchanges: ExchangeInfo[];
}

export interface CallView {
  seq: number;
  method: string;
  path: string;
  occurrence: number;
  key: string;
  headers: Record<string, string>;
  request: any;
  response?: {
    status: number;
    content_type: string;
    latency_us: number;
    streamed: boolean;
    from_recording: boolean;
    chunks: number;
    body: any;
    timing: [number, number][];
  };
}

export interface FileEntry {
  path: string;
  size: number;
  id: string;
  stored: boolean;
  symlink: boolean;
}

export interface FileContent {
  path: string;
  size?: number;
  stored?: boolean;
  binary?: boolean;
  text: string | null;
  missing?: boolean;
}

export interface FileChange {
  kind: "A" | "M" | "D";
  path: string;
  before?: string;
  after?: string;
}

export interface RunDiffCall {
  kind: "same" | "same-request" | "changed" | "only-a" | "only-b";
  label: string;
  a_seq?: number;
  b_seq?: number;
  field?: string;
  a?: string;
  b?: string;
  workspace_before: string[];
}

export interface RunDiffView {
  identical: boolean;
  same: number;
  a_calls: number;
  b_calls: number;
  a_tokens: number;
  b_tokens: number;
  a_model_us: number;
  b_model_us: number;
  first_divergence: number | null;
  calls: RunDiffCall[];
  workspace_end: string[];
}

/** A run loaded into the viewer: a .khn (explored through the wasm kernel) or an imported trace. */
export interface LoadedRun {
  name: string;
  source: "khn" | "otel";
  view: RecordingView;
  handle?: number;       // wasm handle (khn)
  calls?: CallView[];    // full call details (otel: computed in JS)
}
