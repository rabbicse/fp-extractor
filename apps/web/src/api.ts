import type { Minutia, Point } from "./geometry";

export type Step = { name: string; ms: number; bytes: number };
export type Inspection = {
  kind: "image" | "template";
  source_format: string;
  width: number;
  height: number;
  dpi?: number;
  minutiae: Minutia[];
  steps: Step[];
  template_ansi: string;
  template_iso: string;
  preview_bmp?: string;
  wsq?: string;
};
export type CompareResult = {
  score: number;
  threshold: number;
  match: boolean;
  rotation_deg: number;
  pairs: { a: Point; b: Point }[];
};
export type CrossMatch = { names: string[]; scores: number[][]; threshold: number; elapsed_ms: number };
export type Candidate = { id: string; score: number; match: boolean; fingers_matched: number };
export type IdentifyResult = {
  match: boolean;
  id: string | null;
  candidates: Candidate[];
  compared: number;
  elapsed_ms: number;
  fingers_presented: number;
};
export type Stats = { version: string; subjects: number; templates: number; memory_mb: number; match_threads: number };
export type FingerInput = { template: string; position: number };

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
    method,
    headers: { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  let data: { error?: string } = {};
  try {
    data = text ? JSON.parse(text) : {};
  } catch {
    throw new Error(`The server sent an unreadable response (status ${res.status}).`);
  }
  if (!res.ok) throw new Error(data.error ?? `The server returned status ${res.status}.`);
  return data as T;
}

export const api = {
  stats: () => request<Stats>("GET", "/v1/stats"),
  synthetic: (finger_id: number, impression: number) =>
    request<{ file: string }>("POST", "/v1/demo/synthetic", { finger_id, impression }),
  inspect: (file: string) => request<Inspection>("POST", "/v1/inspect", { file }),
  compare: (a: string, b: string) => request<CompareResult>("POST", "/v1/compare", { a, b }),
  crossmatch: (files: { name: string; file: string }[]) => request<CrossMatch>("POST", "/v1/crossmatch", { files }),
  enrol: (id: string, fingers: FingerInput[]) =>
    request<unknown>("PUT", `/v1/subjects/${encodeURIComponent(id)}`, { fingers }),
  identify: (fingers: FingerInput[]) => request<IdentifyResult>("POST", "/v1/identify", { fingers, top_k: 5 }),
};

export function fileToBase64(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error(`${file.name} could not be read.`));
    reader.onload = () => resolve(String(reader.result).split(",")[1] ?? "");
    reader.readAsDataURL(file);
  });
}

export function downloadBase64(name: string, base64: string) {
  const link = document.createElement("a");
  link.href = `data:application/octet-stream;base64,${base64}`;
  link.download = name;
  link.click();
}
