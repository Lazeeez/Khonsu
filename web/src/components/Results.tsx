// Measured against the alternatives: the committed outputs of bench/compare/ and study/
// (docs/RESULTS.md has the method). Imported at build time, so the page shows exactly
// what the repository's scripts produced.
import langgraph from "../../../bench/results/langgraph.json";
import vcr from "../../../bench/results/vcr.json";
import study from "../../../study/results.json";
import { fmtBytes } from "../lib/ui";

type Row = [string, string, string];

function Table({ head, rows }: { head: [string, string, string]; rows: Row[] }) {
  return (
    <table className="data results-table">
      <thead>
        <tr>{head.map((h) => <th key={h}>{h}</th>)}</tr>
      </thead>
      <tbody>
        {rows.map(([k, a, b]) => (
          <tr key={k}>
            <td>{k}</td>
            <td style={{ color: "var(--accent)" }}>{a}</td>
            <td>{b}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function Results() {
  const L = vcr.latency;
  const pc = vcr.prompt_change_at_call_15_of_20;
  const lgLast = langgraph.storage_growth[langgraph.storage_growth.length - 1];
  const o = langgraph.overhead_10_turns;
  const a = study.aggregate;
  const pct = (x: number, y: number) => `${Math.round((100 * x) / Math.max(1, y))}%`;
  return (
    <div className="panel results">
      <div className="panel-h">
        <h2>Against the alternatives</h2>
      </div>
      <div className="panel-b results-grid">
        <section>
          <h3>A real agent on a local model</h3>
          <p className="muted small">
            {study.model} via Ollama on a laptop GPU · {a.tasks} coding tasks · function calling, a real test runner
          </p>
          <div className="tiles">
            <div className="tile"><div className="k">live re-runs that differ</div><div className="v">{a.live_runs_differing_from_recording}/{a.live_runs_total}</div><div className="s">same task, same prompt</div></div>
            <div className="tile"><div className="k">tasks that flip pass ↔ fail</div><div className="v">{a.pass_outcome_flips_between_live_runs}/{a.tasks}</div><div className="s">between identical live runs</div></div>
            <div className="tile"><div className="k">replays identical</div><div className="v" style={{ color: "var(--good)" }}>{a.replays_identical_to_recording}/{a.replays_total}</div><div className="s">model server stopped</div></div>
            <div className="tile"><div className="k">tokens reused on a prompt edit</div><div className="v">{pct(a.edit_tokens_reused, a.edit_tokens_total)}</div><div className="s">{a.edit_calls_reused}/{a.edit_model_calls} calls from the recording</div></div>
          </div>
        </section>
        <section>
          <h3>vs HTTP cassettes (vcrpy {vcr.calls} calls)</h3>
          <Table
            head={["", "Khonsu", "vcrpy"]}
            rows={[
              ["record, p50 per call", `${L.khonsu_record.p50_ms.toFixed(2)} ms`, `${L.vcrpy_record.p50_ms.toFixed(2)} ms`],
              ["replay, p50 per call", `${L.khonsu_replay.p50_ms.toFixed(2)} ms`, `${L.vcrpy_playback.p50_ms.toFixed(2)} ms`],
              ["recording size", fmtBytes(vcr.storage_bytes.khonsu), fmtBytes(vcr.storage_bytes.vcrpy)],
              ["API key written to disk", vcr.api_key_in_recording.khonsu ? "yes" : "no", vcr.api_key_in_recording.vcrpy ? "yes" : "no"],
              ["calls kept after kill -9 at call 30", `${vcr.kill_minus_9.khonsu.calls_in_recording}`, `${vcr.kill_minus_9.vcrpy.calls_in_recording}`],
              ["prompt edited at call 15 of 20", `${pc.khonsu.served_from_recording} replayed, ${pc.khonsu.went_live} live`, pc.vcrpy_record_mode_none.agent_succeeded ? "ok" : "run fails"],
            ]}
          />
        </section>
        <section>
          <h3>vs LangGraph checkpointers</h3>
          <Table
            head={["", "Khonsu", "LangGraph"]}
            rows={[
              [`time travel, ${langgraph.time_travel.trials} resumes`, `${langgraph.time_travel.khonsu_replay_distinct_outcomes} outcome`, `${langgraph.time_travel.langgraph_resume_distinct_outcomes} outcomes`],
              [`storage at ${lgLast.turns} turns`, fmtBytes(lgLast.khonsu_recording_bytes), `${fmtBytes(lgLast.langgraph_sqlite_payload_bytes)} (SQLite)`],
              ["10 turns, on top of InMemorySaver", `+${Math.round((o.inmemory_saver_plus_khonsu_record_s - o.inmemory_saver_s) * 1000)} ms`, `+${Math.round((o.sqlite_saver_s - o.inmemory_saver_s) * 1000)} ms (SqliteSaver)`],
              ["framework", "any (HTTP boundary)", "LangGraph only"],
              ["workspace files", "snapshotted per call", "not captured"],
            ]}
          />
        </section>
      </div>
    </div>
  );
}
