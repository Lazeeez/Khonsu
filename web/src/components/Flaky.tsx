// The landing page's evidence: the same agent run three times on the same task, next to
// three replays of the first run with the model switched off. Data: study/results.json,
// loaded as its own chunk so it never delays the first paint.
import { useEffect, useState } from "react";

interface Outcome { passed: boolean; code_sha?: string; summary?: string }
interface Task { recorded: Outcome; live_reruns: Outcome[]; replays: Outcome[] }
interface Study {
  model: string;
  tasks: Record<string, Task>;
  aggregate: {
    tasks: number;
    pass_outcome_flips_between_live_runs: number;
    replays_identical_to_recording: number;
    replays_total: number;
    live_runs_differing_from_recording: number;
    live_runs_total: number;
  };
}

function Phase({ o, replay, same }: { o: Outcome; replay?: boolean; same?: boolean }) {
  const label = `${o.passed ? "tests passed" : "tests failed"}${replay ? (same ? ", identical to the recorded run" : ", differs") : ""}`;
  return <span className={`phase ${o.passed ? "pass" : "fail"} ${replay ? "replay" : ""}`} role="img" aria-label={label} title={label} />;
}

export function Flaky() {
  const [study, setStudy] = useState<Study | null>(null);
  useEffect(() => {
    let live = true;
    import("../../../study/results.json").then((m) => live && setStudy(m.default as unknown as Study));
    return () => {
      live = false;
    };
  }, []);
  if (!study) return <section className="flaky placeholder" aria-hidden />;
  const a = study.aggregate;
  const same = (x: Outcome, y: Outcome) => x.code_sha === y.code_sha && x.summary === y.summary && x.passed === y.passed;
  return (
    <section className="flaky" aria-label="The same agent, three times">
      <div className="flaky-copy">
        <p className="eyebrow">Why record</p>
        <h2 className="display small-display">The same agent, the same task, three times.</h2>
        <p className="lede">
          A coding agent on {study.model} wrote and tested code for {a.tasks} tasks. Run again with the identical prompt, it
          wrote different code every time ({a.live_runs_differing_from_recording} of {a.live_runs_total}), and on{" "}
          <b>{a.pass_outcome_flips_between_live_runs} of {a.tasks} tasks</b> the tests flipped between pass and fail. A bug you
          saw once is gone on the next run.
        </p>
        <p className="lede">
          Khonsu's replays of the first run never moved: <b className="gold-t">{a.replays_identical_to_recording} of {a.replays_total}</b>{" "}
          were identical, with the model server switched off.{" "}
          <a href="#/bench">See every result →</a>
        </p>
      </div>
      <div className="flaky-grid" role="table" aria-label="Test outcome per run">
        <div className="fg-row fg-head" role="row">
          <span role="columnheader">task</span>
          <span role="columnheader">live runs</span>
          <span role="columnheader">replays, model off</span>
        </div>
        {Object.entries(study.tasks).map(([name, t]) => (
          <div className="fg-row" role="row" key={name}>
            <span className="mono" role="rowheader">{name}</span>
            <span className="fg-cells" role="cell">
              {[t.recorded, ...t.live_reruns].map((o, i) => <Phase key={i} o={o} />)}
            </span>
            <span className="fg-cells" role="cell">
              {t.replays.map((o, i) => <Phase key={i} o={o} replay same={same(o, t.recorded)} />)}
            </span>
          </div>
        ))}
        <div className="legend fg-legend">
          <span><span className="phase pass" /> tests pass</span>
          <span><span className="phase fail" /> tests fail</span>
          <span><span className="phase pass replay" /> replay, identical to the first run</span>
        </div>
      </div>
    </section>
  );
}
