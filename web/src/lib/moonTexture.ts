// A procedural full moon, painted once into a canvas and cached as a data URL: the near
// side's maria in roughly their real places, craters with lit and shadowed rims, Tycho's
// ray system, fine regolith grain, and limb darkening. Deterministic (seeded), ~5 ms,
// no network. The dial masks it with a soft-terminator shadow for the phase.

let cached: string | null = null;

function rng(seed: number) {
  return () => {
    seed = (seed * 16807) % 2147483647;
    return (seed - 1) / 2147483646;
  };
}

// Maria in unit coordinates (x, y in [-1, 1], y down), as clusters of soft blobs.
const MARIA: [number, number, number, number][] = [
  // Oceanus Procellarum (west)
  [-0.55, -0.1, 0.34, 0.5], [-0.62, 0.22, 0.26, 0.42], [-0.42, -0.38, 0.22, 0.3],
  // Mare Imbrium
  [-0.25, -0.42, 0.3, 0.55], [-0.08, -0.5, 0.2, 0.45],
  // Mare Serenitatis
  [0.2, -0.36, 0.17, 0.62],
  // Mare Tranquillitatis
  [0.3, -0.05, 0.2, 0.6], [0.42, 0.06, 0.15, 0.5],
  // Mare Crisium
  [0.68, -0.22, 0.1, 0.72],
  // Mare Fecunditatis / Nectaris
  [0.55, 0.2, 0.13, 0.5], [0.34, 0.26, 0.09, 0.45],
  // Mare Nubium / Cognitum
  [-0.18, 0.3, 0.2, 0.42], [-0.32, 0.12, 0.16, 0.4],
  // Mare Humorum
  [-0.52, 0.42, 0.1, 0.5],
  // Mare Frigoris (north band)
  [-0.05, -0.74, 0.26, 0.28], [0.25, -0.7, 0.18, 0.25],
];

export function moonTexture(size = 512): string {
  if (cached) return cached;
  if (typeof document === "undefined") return "";
  const c = document.createElement("canvas");
  c.width = c.height = size;
  const g = c.getContext("2d");
  if (!g) return "";
  const r = size / 2;
  const P = (u: number) => r + u * r;  // unit -> pixels
  const rand = rng(1969);

  g.save();
  g.beginPath();
  g.arc(r, r, r, 0, Math.PI * 2);
  g.clip();

  // Highlands: warm, slightly golden silver, brighter toward the sub-solar point.
  const base = g.createRadialGradient(P(-0.25), P(-0.3), r * 0.1, r, r, r * 1.05);
  base.addColorStop(0, "#fbf5e6");
  base.addColorStop(0.55, "#e8dfc9");
  base.addColorStop(1, "#c7bda6");
  g.fillStyle = base;
  g.fillRect(0, 0, size, size);

  // Maria: soft grey-blue basalt, built from overlapping blobs with irregular edges.
  for (const [x, y, s, a] of MARIA) {
    for (let k = 0; k < 7; k++) {
      const jx = x + (rand() - 0.5) * s * 0.9, jy = y + (rand() - 0.5) * s * 0.9;
      const rad = r * s * (0.55 + rand() * 0.6);
      const grd = g.createRadialGradient(P(jx), P(jy), 0, P(jx), P(jy), rad);
      grd.addColorStop(0, `rgba(88, 92, 98, ${a * 0.62})`);
      grd.addColorStop(0.6, `rgba(104, 106, 108, ${a * 0.32})`);
      grd.addColorStop(1, "rgba(110, 110, 110, 0)");
      g.fillStyle = grd;
      g.beginPath();
      g.arc(P(jx), P(jy), rad, 0, Math.PI * 2);
      g.fill();
    }
  }

  // Craters: a shaded bowl, a bright rim toward the light (upper left), dark rim opposite.
  const crater = (x: number, y: number, rad: number, depth: number) => {
    const cx = P(x), cy = P(y);
    const bowl = g.createRadialGradient(cx + rad * 0.25, cy + rad * 0.25, rad * 0.1, cx, cy, rad);
    bowl.addColorStop(0, `rgba(70, 66, 60, ${0.12 * depth})`);
    bowl.addColorStop(1, `rgba(70, 66, 60, ${0.03 * depth})`);
    g.fillStyle = bowl;
    g.beginPath();
    g.arc(cx, cy, rad, 0, Math.PI * 2);
    g.fill();
    g.lineWidth = Math.max(0.6, rad * 0.18);
    g.strokeStyle = `rgba(255, 250, 235, ${0.32 * depth})`;
    g.beginPath();
    g.arc(cx, cy, rad, Math.PI * 0.55, Math.PI * 1.55);  // lit rim: south-west to north-east through west
    g.stroke();
    g.strokeStyle = `rgba(40, 38, 36, ${0.2 * depth})`;
    g.beginPath();
    g.arc(cx, cy, rad, Math.PI * 1.55, Math.PI * 2.55);
    g.stroke();
  };
  for (let i = 0; i < 200; i++) {
    const a = rand() * Math.PI * 2, d = Math.sqrt(rand()) * 0.96;
    const rad = r * (0.006 + Math.pow(rand(), 3.2) * 0.05);
    crater(Math.cos(a) * d, Math.sin(a) * d, rad, 0.5 + rand() * 0.5);
  }
  // Named landmarks: Copernicus, Kepler, Aristarchus (bright), Plato (dark floor), Tycho.
  crater(-0.3, -0.18, r * 0.045, 1);
  crater(-0.55, -0.12, r * 0.028, 0.9);
  crater(-0.68, -0.38, r * 0.022, 0.8);
  crater(-0.12, -0.72, r * 0.04, 1);

  // Tycho and its ray system: bright streaks radiating across the southern highlands.
  const tx = P(-0.12), ty = P(0.68);
  g.lineCap = "round";
  for (let i = 0; i < 22; i++) {
    const ang = rand() * Math.PI * 2, len = r * (0.25 + rand() * 0.75);
    const ray = g.createLinearGradient(tx, ty, tx + Math.cos(ang) * len, ty + Math.sin(ang) * len);
    ray.addColorStop(0, "rgba(255, 252, 240, 0.34)");
    ray.addColorStop(1, "rgba(255, 252, 240, 0)");
    g.strokeStyle = ray;
    g.lineWidth = r * (0.006 + rand() * 0.012);
    g.beginPath();
    g.moveTo(tx, ty);
    g.lineTo(tx + Math.cos(ang) * len, ty + Math.sin(ang) * len);
    g.stroke();
  }
  const halo = g.createRadialGradient(tx, ty, 0, tx, ty, r * 0.07);
  halo.addColorStop(0, "rgba(255, 253, 245, 0.95)");
  halo.addColorStop(1, "rgba(255, 253, 245, 0)");
  g.fillStyle = halo;
  g.beginPath();
  g.arc(tx, ty, r * 0.07, 0, Math.PI * 2);
  g.fill();
  crater(-0.12, 0.68, r * 0.022, 1);

  // Regolith grain.
  const img = g.getImageData(0, 0, size, size);
  const px = img.data;
  for (let i = 0; i < px.length; i += 4) {
    const n = (rand() - 0.5) * 16;
    px[i] += n;
    px[i + 1] += n;
    px[i + 2] += n;
  }
  g.putImageData(img, 0, 0);

  // Limb darkening: the edge of a sphere reflects less light toward us.
  const limb = g.createRadialGradient(r, r, r * 0.55, r, r, r);
  limb.addColorStop(0, "rgba(20, 16, 10, 0)");
  limb.addColorStop(0.85, "rgba(20, 16, 10, 0.18)");
  limb.addColorStop(1, "rgba(20, 16, 10, 0.5)");
  g.fillStyle = limb;
  g.fillRect(0, 0, size, size);
  g.restore();

  try {
    cached = c.toDataURL("image/webp", 0.9);
    if (!cached.startsWith("data:image/webp")) cached = c.toDataURL("image/png");
  } catch {
    cached = "";
  }
  return cached;
}
