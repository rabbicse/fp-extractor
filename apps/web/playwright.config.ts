import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { defineConfig } from "@playwright/test";

const server = process.env.FPX_SERVER;
if (!server) throw new Error("Set FPX_SERVER to the fpx-server binary. Run `npm run build` first.");
const db = join(mkdtempSync(join(tmpdir(), "fpx-e2e-")), "e2e.fpxdb");

export default defineConfig({
  testDir: "e2e",
  timeout: 120_000,
  workers: 1,
  snapshotPathTemplate: "e2e/snapshots/{arg}{ext}",
  expect: { toHaveScreenshot: { maxDiffPixelRatio: 0.02, animations: "disabled" } },
  use: { baseURL: "http://127.0.0.1:8099", viewport: { width: 1360, height: 900 }, colorScheme: "light" },
  webServer: {
    command: `"${server}" --db "${db}" --host 127.0.0.1 --port 8099 --demo --web-root dist`,
    url: "http://127.0.0.1:8099/healthz",
    reuseExistingServer: false,
  },
});
