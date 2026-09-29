// Import OpenTelemetry traces that follow the GenAI semantic conventions (OTLP/JSON),
// as exported by OpenLLMetry, OpenLIT, Langfuse, Phoenix, the OpenAI Agents SDK
// exporters and others. Supports both the older attribute form
// (gen_ai.prompt.N.role/content, gen_ai.completion.N.content) and the newer
// structured form (gen_ai.input.messages / gen_ai.output.messages, events).
// Tool executions (gen_ai.operation.name = execute_tool) become tool calls.
// No workspace data exists in traces, so the viewer shows calls only.

import type { CallView, ExchangeInfo, LoadedRun, RecordingView } from "./types";

type AnyValue = { stringValue?: string; intValue?: string | number; doubleValue?: number; boolValue?: boolean;
  arrayValue?: { values: AnyValue[] }; kvlistValue?: { values: KeyValue[] } };
type KeyValue = { key: string; value: AnyValue };
type Span = {
  name: string; traceId?: string; spanId?: string; startTimeUnixNano: string | number; endTimeUnixNano: string | number;
  attributes?: KeyValue[]; events?: { name: string; timeUnixNano?: string; attributes?: KeyValue[] }[];
};

function val(v: AnyValue | undefined): unknown {
  if (!v) return undefined;
  if (v.stringValue !== undefined) return v.stringValue;
  if (v.intValue !== undefined) return Number(v.intValue);
  if (v.doubleValue !== undefined) return v.doubleValue;
  if (v.boolValue !== undefined) return v.boolValue;
  if (v.arrayValue) return v.arrayValue.values.map(val);
  if (v.kvlistValue) return Object.fromEntries(v.kvlistValue.values.map((kv) => [kv.key, val(kv.value)]));
  return undefined;
}

function attrs(list: KeyValue[] | undefined): Record<string, unknown> {
  return Object.fromEntries((list ?? []).map((kv) => [kv.key, val(kv.value)]));
}

function parseMaybeJson(x: unknown): unknown {
  if (typeof x !== "string") return x;
  try {
    return JSON.parse(x);
  } catch {
    return x;
  }
}

/** Messages from indexed attributes: prefix.0.role, prefix.0.content, … */
function indexed(a: Record<string, unknown>, prefix: string): { role: string; content: unknown }[] {
  const out: { role: string; content: unknown }[] = [];
  for (let i = 0; ; i++) {
    const role = a[`${prefix}.${i}.role`];
    const content = a[`${prefix}.${i}.content`];
    if (role === undefined && content === undefined) break;
    out.push({ role: String(role ?? "assistant"), content: content ?? "" });
  }
  return out;
}

function textOf(content: unknown): string {
  if (typeof content === "string") return content;
  if (Array.isArray(content))
    return content.map((p: any) => (typeof p === "string" ? p : p?.text ?? p?.content ?? "")).join("");
  if (content && typeof content === "object") return (content as any).text ?? JSON.stringify(content);
  return "";
}

const nanos = (x: string | number) => BigInt(String(x));

export function isOtel(json: any): boolean {
  return !!json && Array.isArray(json.resourceSpans);
}

export function importOtel(json: any, name: string): LoadedRun {
  const spans: Span[] = [];
  for (const rs of json.resourceSpans ?? [])
    for (const ss of rs.scopeSpans ?? rs.instrumentationLibrarySpans ?? []) spans.push(...(ss.spans ?? []));

  const relevant = spans
    .map((s) => ({ s, a: attrs(s.attributes) }))
    .filter(({ a }) => Object.keys(a).some((k) => k.startsWith("gen_ai.")))
    .sort((x, y) => (nanos(x.s.startTimeUnixNano) < nanos(y.s.startTimeUnixNano) ? -1 : 1));
  if (relevant.length === 0) throw new Error("no GenAI spans (gen_ai.* attributes) in this trace");

  const t0 = nanos(relevant[0].s.startTimeUnixNano);
  const exchanges: ExchangeInfo[] = [];
  const calls: CallView[] = [];
  let last = t0;

  relevant.forEach(({ s, a }, seq) => {
    const start = nanos(s.startTimeUnixNano);
    const end = nanos(s.endTimeUnixNano);
    if (end > last) last = end;
    const latency_us = Number((end - start) / 1000n);
    const t_us = Number((start - t0) / 1000n);
    const op = String(a["gen_ai.operation.name"] ?? "");
    const tool = op === "execute_tool" || a["gen_ai.tool.name"] !== undefined;

    if (tool) {
      const nameStr = String(a["gen_ai.tool.name"] ?? s.name);
      const args = parseMaybeJson(a["gen_ai.tool.call.arguments"] ?? {});
      const result = parseMaybeJson(a["gen_ai.tool.call.result"] ?? "");
      exchanges.push({ seq, t_us, method: "POST", path: "/khonsu/v1/tool", key: s.spanId ?? String(seq), occurrence: 0,
        tool: true, model: `tool ${nameStr}`, input: JSON.stringify(args), output: typeof result === "string" ? result : JSON.stringify(result),
        tool_calls: nameStr, prompt_tokens: 0, completion_tokens: 0, status: 200, latency_us, workspace_changes: [] });
      calls.push({ seq, method: "POST", path: "/khonsu/v1/tool", occurrence: 0, key: s.spanId ?? "", headers: {},
        request: { name: nameStr, args },
        response: { status: 200, content_type: "application/json", latency_us, streamed: false, from_recording: false,
          chunks: 0, body: { ok: true, result }, timing: [] } });
      return;
    }

    // Input messages: structured attribute, indexed attributes, or events.
    let messages: { role: string; content: unknown }[] = [];
    const structuredIn = parseMaybeJson(a["gen_ai.input.messages"]);
    if (Array.isArray(structuredIn)) {
      messages = structuredIn.map((m: any) => ({ role: m.role ?? "user", content: m.parts ? textOf(m.parts) : m.content }));
    } else {
      messages = indexed(a, "gen_ai.prompt");
    }
    if (messages.length === 0)
      for (const ev of s.events ?? []) {
        const m = /^gen_ai\.(system|user|assistant|tool)\.message$/.exec(ev.name);
        if (m) messages.push({ role: m[1], content: attrs(ev.attributes)["content"] ?? "" });
      }
    const sys = a["gen_ai.system_instructions"];
    if (sys && !messages.some((m) => m.role === "system")) messages.unshift({ role: "system", content: textOf(parseMaybeJson(sys)) });

    let output = "";
    const toolCalls: string[] = [];
    const structuredOut = parseMaybeJson(a["gen_ai.output.messages"]);
    if (Array.isArray(structuredOut)) {
      for (const m of structuredOut as any[]) {
        for (const p of m.parts ?? []) {
          if (p.type === "tool_call") toolCalls.push(`${p.name}(${JSON.stringify(p.arguments ?? {})})`);
          else output += p.content ?? p.text ?? "";
        }
        if (!m.parts) output += textOf(m.content);
      }
    } else {
      output = indexed(a, "gen_ai.completion").map((m) => textOf(m.content)).join("\n");
      for (let i = 0; a[`gen_ai.completion.0.tool_calls.${i}.name`] !== undefined; i++)
        toolCalls.push(`${a[`gen_ai.completion.0.tool_calls.${i}.name`]}(${a[`gen_ai.completion.0.tool_calls.${i}.arguments`] ?? ""})`);
    }
    if (!output)
      for (const ev of s.events ?? [])
        if (ev.name === "gen_ai.choice") output += textOf(parseMaybeJson(attrs(ev.attributes)["message"] ?? attrs(ev.attributes)["content"]));

    const model = String(a["gen_ai.request.model"] ?? a["gen_ai.response.model"] ?? s.name);
    const pt = Number(a["gen_ai.usage.input_tokens"] ?? a["gen_ai.usage.prompt_tokens"] ?? 0);
    const ct = Number(a["gen_ai.usage.output_tokens"] ?? a["gen_ai.usage.completion_tokens"] ?? 0);
    const lastUser = [...messages].reverse().find((m) => m.role === "user");
    exchanges.push({ seq, t_us, method: "POST", path: "/v1/chat/completions", key: s.spanId ?? String(seq), occurrence: 0,
      tool: false, model, input: textOf(lastUser?.content ?? ""), output, tool_calls: toolCalls.join(", "),
      prompt_tokens: pt, completion_tokens: ct, status: 200, latency_us, workspace_changes: [] });
    calls.push({ seq, method: "POST", path: "/v1/chat/completions", occurrence: 0, key: s.spanId ?? "", headers: {},
      request: { model, messages: messages.map((m) => ({ role: m.role, content: textOf(m.content) })),
        temperature: a["gen_ai.request.temperature"] },
      response: { status: 200, content_type: "application/json", latency_us, streamed: false, from_recording: false, chunks: 0,
        body: { choices: [{ message: { role: "assistant", content: output,
          tool_calls: toolCalls.map((t, i) => ({ id: `call_${i}`, type: "function",
            function: { name: t.slice(0, t.indexOf("(")), arguments: t.slice(t.indexOf("(") + 1, -1) } })) } }],
          usage: { prompt_tokens: pt, completion_tokens: ct } }, timing: [] } });
  });

  const view: RecordingView = {
    format: "opentelemetry-genai", mode: "trace", command: String(json.resourceSpans?.[0]?.resource?.attributes
      ?.find((kv: KeyValue) => kv.key === "service.name")?.value?.stringValue ?? name),
    cwd: "", upstream: "", parent: "", workspace: "", tool_version: "", snapshots: 0, objects: 0, object_bytes: 0,
    started_unix_ms: Number(t0 / 1_000_000n), digest: "", chain_ok: true, complete: true, recovery: "", frames: 0,
    duration_us: Number((last - t0) / 1000n), snapshot_list: [], exchanges,
  };
  return { name, source: "otel", view, calls };
}
