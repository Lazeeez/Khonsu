// The loading indicator: the textured moon turning slowly while its phase sweeps from new to
// full and back, the way a month passes. Only transforms animate (compositor work), so it
// stays smooth even while the main thread is busy loading the page it stands in for.
import { useMemo } from "react";
import { moonTexture } from "../lib/moonTexture";

export function MoonLoader({ label = "Loading", size = 88, inline = false }: { label?: string; size?: number; inline?: boolean }) {
  const tex = useMemo(() => moonTexture(), []);
  return (
    <div className={inline ? "moonloader inline" : "loading"} role="status" aria-live="polite">
      <div className="ml-moon" style={{ width: size, height: size }} aria-hidden>
        <div className="ml-halo" />
        <div className="ml-disc">
          <div className="ml-surface" style={tex ? { backgroundImage: `url(${tex})` } : undefined} />
          <div className="ml-shadow" />
          <div className="ml-limb" />
        </div>
      </div>
      <div className="ml-label">{label}<span className="ml-dots" aria-hidden><i>.</i><i>.</i><i>.</i></span></div>
    </div>
  );
}
