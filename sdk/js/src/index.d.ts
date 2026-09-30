export declare const version: string;

/** A tool threw an error of a type that is not a JavaScript builtin; replayed by name and message. */
export declare class ToolError extends Error {
  constructor(typeName: string, message: string);
  readonly typeName: string;
  readonly toolMessage: string;
}

/** Replay met a tool call that is not in the recording (its arguments differ). */
export declare class KhonsuDivergence extends Error {
  constructor(message: string);
}

export interface ToolOptions {
  /**
   * "skip" (default): on replay return the recorded result without running the tool.
   * "rerun": run it again for its effect, report a changed result, return the recorded one.
   */
  replay?: "skip" | "rerun";
}

export type KhonsuTool<A extends unknown[], R> = ((...args: A) => Promise<Awaited<R>>) & { readonly khonsuTool: string };

/** Record and replay a tool's calls. Arguments and result must be JSON-compatible. */
export declare function tool<A extends unknown[], R>(name: string, fn: (...args: A) => R, options?: ToolOptions): KhonsuTool<A, R>;

/** True under `khonsu record` / `replay` / `fork`. */
export declare function active(): boolean;

/** "record", "replay" or null. */
export declare function mode(): "record" | "replay" | null;

declare const _default: {
  tool: typeof tool;
  active: typeof active;
  mode: typeof mode;
  ToolError: typeof ToolError;
  KhonsuDivergence: typeof KhonsuDivergence;
  version: string;
};
export default _default;
