import react from "@vitejs/plugin-react";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [react()],
  server: { proxy: { "/v1": "http://127.0.0.1:8080", "/healthz": "http://127.0.0.1:8080" } },
  test: { include: ["src/**/*.test.ts"] },
});
