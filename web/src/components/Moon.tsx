import { useId } from "react";

// Khonsu's mark: the crescent of the moon god, in lapis-night gold. Each instance gets
// its own gradient id: duplicate SVG ids break rendering in some browsers when one unmounts.
export function Moon({ size = 26 }: { size?: number }) {
  const id = `mg-${useId().replace(/:/g, "")}`;
  return (
    <svg width={size} height={size} viewBox="0 0 64 64" aria-hidden className="moon-mark">
      <defs>
        <linearGradient id={id} x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#fff3d6" />
          <stop offset="1" stopColor="#f5c26b" />
        </linearGradient>
      </defs>
      <path d="M40 8a24 24 0 1 0 16 40A19 19 0 1 1 40 8z" fill={`url(#${id})`} />
    </svg>
  );
}
