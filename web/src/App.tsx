import { Suspense, lazy, useEffect, useState } from "react";
import { Moon } from "./components/Moon";
import { Landing } from "./Landing";

// Heavy pages load on demand: the landing page ships no WebAssembly and no canvas code.
const Viewer = lazy(() => import("./viewer/Viewer").then((m) => ({ default: m.Viewer })));
const Simulator = lazy(() => import("./Simulator").then((m) => ({ default: m.Simulator })));
const Play = lazy(() => import("./play/Play").then((m) => ({ default: m.Play })));
const Bench = lazy(() => import("./components/Bench").then((m) => ({ default: m.Bench })));

const REPO_URL: string | undefined = import.meta.env.VITE_REPO_URL;

type Route = "home" | "viewer" | "play" | "simulator" | "bench";

function routeOf(hash: string): Route {
  if (hash.startsWith("#/viewer")) return "viewer";
  if (hash.startsWith("#/play")) return "play";
  if (hash.startsWith("#/simulator")) return "simulator";
  if (hash.startsWith("#/bench")) return "bench";
  return "home";
}

export default function App() {
  const [route, setRoute] = useState<Route>(() => routeOf(location.hash));
  useEffect(() => {
    const on = () => {
      setRoute(routeOf(location.hash));
      window.scrollTo(0, 0);
    };
    addEventListener("hashchange", on);
    return () => removeEventListener("hashchange", on);
  }, []);

  const link = (r: Route, href: string, label: string) => (
    <a href={href} className={route === r ? "active" : ""} aria-current={route === r ? "page" : undefined}>{label}</a>
  );

  return (
    <div className="app">
      <header className="topbar">
        <a className="brand" href="#/">
          <Moon />
          <div>
            <h1>KHONSU</h1>
            <div className="sub">time-travel debugger for AI agents</div>
          </div>
        </a>
        <nav className="nav">
          {link("viewer", "#/viewer", "Viewer")}
          {link("play", "#/play", "Play")}
          {link("simulator", "#/simulator", "Simulator")}
          {link("bench", "#/bench", "Benchmarks")}
          {REPO_URL && <a href={`${REPO_URL}#readme`} target="_blank" rel="noreferrer">Docs</a>}
          {REPO_URL && <a href={REPO_URL} target="_blank" rel="noreferrer">GitHub</a>}
        </nav>
      </header>
      <Suspense fallback={<div className="loading"><div className="moon-spin"><Moon size={44} /></div><div>Loading…</div></div>}>
        {route === "home" && <Landing />}
        {route === "viewer" && <Viewer />}
        {route === "play" && <Play />}
        {route === "simulator" && <Simulator />}
        {route === "bench" && <Bench />}
      </Suspense>
    </div>
  );
}
