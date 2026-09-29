// Shapes returned by the kh_lune_* bindings (platform/web/bindings.cpp).

export interface VmState {
  id: number;
  parent: number;
  status: "running" | "halted" | "error" | "out of gas";
  error: string;
  steps: number;
  gas_left: number;
  line: number;
  printed: number;
  pages_copied: number;
  dirty_pages: number;
  output: string[];
  vars: [string, string][];
}

export interface ForkInfo {
  id: number;
  dirty_pages_committed: number;
  fork_ms: number;
  fork_committed_us: number;
  root: string;
}

export interface SpecBranch {
  id: number;
  value: number;
  status: VmState["status"];
  steps: number;
  pages_copied: number;
  last: string;
}

export interface SpecResult {
  branches: SpecBranch[];
  ms: number;
}

export interface ArrayPages {
  length: number;
  pages: number;
  shared: number;
  copied: number;
  new: number;
  map: string; // per page vs the fork point: s = shared, c = copied, n = new, . = never written
}

export interface PageReport {
  arrays: ArrayPages[];
  store_pages: number;
  store_bytes: number;
}
