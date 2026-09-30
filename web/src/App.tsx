import { Suspense, lazy, useEffect, useState } from "react";
import { Moon } from "./components/Moon";
import { MoonLoader } from "./components/MoonLoader";
import { Sky } from "./components/Sky";
import { Landing } from "./Landing";

// Heavy pages load on demand: the landing page ships no WebAssembly and no canvas code.
// Each loader is also called ahead of time (idle, or on hover/touch of its link), so a
// click almost never waits on the network.
const loaders = {
  guide: () => import("./guide/Guide").then((m) => ({ default: m.Guide })),
  downloads: () => import("./downloads/Downloads").then((m) => ({ default: m.Downloads })),
  viewer: () => import("./viewer/Viewer").then((m) => ({ default: m.Viewer })),
  play: () => import("./play/Play").then((m) => ({ default: m.Play })),
  simulator: () => import("./Simulator").then((m) => ({ default: m.Simulator })),
  bench: () => import("./components/Bench").then((m) => ({ default: m.Bench })),
};
const Guide = lazy(loaders.guide);
const Downloads = lazy(loaders.downloads);
const Viewer = lazy(loaders.viewer);
const Play = lazy(loaders.play);
const Simulator = lazy(loaders.simulator);
const Bench = lazy(loaders.bench);

type Route = "home" | keyof typeof loaders;

function routeOf(hash: string): Route {
  if (hash.startsWith("#/guide")) return "guide";
  if (hash.startsWith("#/downloads")) return "downloads";
  if (hash.startsWith("#/viewer")) return "viewer";
  if (hash.startsWith("#/play")) return "play";
  if (hash.startsWith("#/simulator")) return "simulator";
  if (hash.startsWith("#/bench")) return "bench";
  return "home";
}

const prefetched = new Set<Route>();
function prefetch(r: Route) {
  if (r === "home" || prefetched.has(r)) return;
  prefetched.add(r);
  loaders[r]().catch(() => prefetched.delete(r));
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

  // Warm the other pages once the current one is idle.
  useEffect(() => {
    const idle = (cb: () => void) =>
      "requestIdleCallback" in window ? requestIdleCallback(cb, { timeout: 4000 }) : setTimeout(cb, 2500);
    const id = idle(() => (["guide", "downloads", "viewer", "play", "bench", "simulator"] as Route[]).forEach(prefetch));
    return () => ("cancelIdleCallback" in window ? cancelIdleCallback(id as number) : clearTimeout(id as number));
  }, []);

  const link = (r: Route, label: string) => (
    <a href={`#/${r}`} className={route === r ? "active" : ""} aria-current={route === r ? "page" : undefined}
      onPointerEnter={() => prefetch(r)} onTouchStart={() => prefetch(r)} onFocus={() => prefetch(r)}>
      {label}
    </a>
  );

  return (
    <div className="app">
      <Sky />
      <div className="film" aria-hidden />
      <header className="topbar">
        <a className="brand" href="#/">
          <Moon />
          <div>
            <h1>KHONSU</h1>
            <div className="sub">time-travel debugger for AI agents</div>
          </div>
        </a>
        <nav className="nav" aria-label="Main">
          {link("guide", "Guide")}
          {link("viewer", "Viewer")}
          {link("play", "Play")}
          {link("simulator", "Simulator")}
          {link("bench", "Benchmarks")}
          {link("downloads", "Download")}
        </nav>
      </header>
      <Suspense fallback={<MoonLoader />}>
        <div className="route" key={route}>
          {route === "home" && <Landing />}
          {route === "guide" && <Guide />}
          {route === "downloads" && <Downloads />}
          {route === "viewer" && <Viewer />}
          {route === "play" && <Play />}
          {route === "simulator" && <Simulator />}
          {route === "bench" && <Bench />}
        </div>
      </Suspense>
    </div>
  );
}
