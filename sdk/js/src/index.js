// Khonsu JavaScript/TypeScript SDK: make tool side effects replayable.
//
//   import { tool } from "khonsu";
//   const sendEmail = tool("sendEmail", async (to, body) => { ... });
//
// Under `khonsu record` the tool runs and its arguments and result are recorded. Under
// `khonsu replay` the recorded result is returned without running it, so side effects
// are not repeated. `{ replay: "rerun" }` runs it again for its effect (a file write the
// agent reads back), reports a changed result, and still returns the recorded one.
// Outside Khonsu the wrapper just calls the function. Same protocol as the Python SDK
// (platform/native/proxy.cpp): POST $KHONSU_TOOL_URL/begin, then /end. Node >= 18.

export const version = "0.3.0";

/** A tool threw an error of a type that is not a JavaScript builtin; replayed by name and message. */
export class ToolError extends Error {
  constructor(typeName, message) {
    super(`${typeName}: ${message}`);
    this.name = "ToolError";
    this.typeName = typeName;
    this.toolMessage = message;
  }
}

/** Replay met a tool call that is not in the recording (its arguments differ). */
export class KhonsuDivergence extends Error {
  constructor(message) {
    super(message);
    this.name = "KhonsuDivergence";
  }
}

const url = () => (typeof process !== "undefined" && process.env.KHONSU_TOOL_URL) || null;

/** True under `khonsu record` / `replay` / `fork`. */
export const active = () => url() !== null;

/** "record", "replay" or null. */
export const mode = () => (active() ? process.env.KHONSU_MODE ?? null : null);

const BUILTIN = { Error, TypeError, RangeError, SyntaxError, ReferenceError, EvalError, URIError };

// Round-trip now so record and replay see exactly the same value (undefined -> null in arrays, dropped in objects).
function jsonable(value, what) {
  try {
    const s = JSON.stringify(value === undefined ? null : value);
    return JSON.parse(s);
  } catch (e) {
    throw new TypeError(`khonsu.tool: ${what} must be JSON-serializable (${e.message})`);
  }
}

// Key-order-independent JSON, for comparing a re-run result with the recorded (canonical) one.
function canonical(v) {
  if (Array.isArray(v)) return `[${v.map(canonical).join(",")}]`;
  if (v && typeof v === "object")
    return `{${Object.keys(v).sort().map((k) => `${JSON.stringify(k)}:${canonical(v[k])}`).join(",")}}`;
  return JSON.stringify(v);
}

async function post(path, payload) {
  const res = await fetch(url() + path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  const body = await res.text();
  if (res.status === 409) {
    let msg = body;
    try {
      msg = JSON.parse(body).error.message;
    } catch {}
    throw new KhonsuDivergence(msg);
  }
  if (!res.ok) throw new Error(`khonsu proxy error ${res.status}: ${body}`);
  return JSON.parse(body);
}

function raiseRecorded(error = {}) {
  const name = error.type ?? "Error";
  const message = error.message ?? "";
  const Ctor = BUILTIN[name];
  if (Ctor) throw new Ctor(message);
  throw new ToolError(name, message);
}

function outcomeOf(err) {
  return { ok: false, error: { type: err?.name ?? "Error", message: String(err?.message ?? err) } };
}

/**
 * Wrap `fn` so its calls are recorded and replayed. The wrapper always returns a Promise.
 * @param {string} name  a stable name for the tool (it is part of the match key)
 * @param {Function} fn  sync or async; arguments and result must be JSON-compatible
 * @param {{replay?: "skip" | "rerun"}} [options]
 */
export function tool(name, fn, options = {}) {
  const replay = options.replay ?? "skip";
  if (typeof name !== "string" || !name) throw new TypeError("khonsu.tool: a tool needs a name");
  if (typeof fn !== "function") throw new TypeError("khonsu.tool: fn must be a function");
  if (replay !== "skip" && replay !== "rerun") throw new TypeError('khonsu.tool: replay must be "skip" or "rerun"');

  const wrapped = async function (...args) {
    if (!active()) return fn.apply(this, args);
    const answer = await post("/begin", { name, args: jsonable(args, "arguments") });
    if (answer.action === "replay") {
      const recorded = answer.outcome ?? {};
      if (replay === "rerun") {
        // Run again for the effect; hand the agent the recorded result so the replay
        // stays on the recorded path (timings, PIDs, ... differ).
        let fresh;
        try {
          fresh = { ok: true, result: jsonable(await fn.apply(this, args), "the return value") };
        } catch (e) {
          fresh = outcomeOf(e);
        }
        const now = fresh.ok ? { ok: true, result: fresh.result } : { ok: false, error: fresh.error };
        const was = fresh.ok ? { ok: recorded.ok, result: recorded.result } : { ok: recorded.ok, error: recorded.error };
        if (canonical(now) !== canonical(was))
          console.error(`[khonsu] tool ${name} re-ran with a different result than recorded; returning the recorded one`);
      }
      if (recorded.ok) return recorded.result;
      raiseRecorded(recorded.error);
    }
    const token = answer.token;
    let result;
    try {
      result = jsonable(await fn.apply(this, args), "the return value");
    } catch (e) {  // includes a result that is not JSON: the call is closed either way
      await post("/end", { token, outcome: outcomeOf(e) });
      throw e;
    }
    await post("/end", { token, outcome: { ok: true, result } });  // durable before the tool returns
    return result;
  };
  Object.defineProperty(wrapped, "name", { value: fn.name || name });
  wrapped.khonsuTool = name;
  return wrapped;
}

export default { tool, active, mode, ToolError, KhonsuDivergence, version };
