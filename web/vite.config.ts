import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// GitHub Pages serves the site from /<repo>/; override with KHONSU_BASE for other hosts.
export default defineConfig({
  base: process.env.KHONSU_BASE ?? "/Khonsu/",
  plugins: [react()],
  worker: { format: "es" },
  build: { target: "es2022", sourcemap: true },
});
