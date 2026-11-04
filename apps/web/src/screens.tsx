import { useEffect, useState } from "react";
import { api, downloadBase64 } from "./api";
import type { CompareResult, CrossMatch, IdentifyResult, Stats } from "./api";
import { heat, minutiaClass } from "./geometry";
import { ComparePlates, Plate } from "./Plates";
import benchmarks from "./benchmarks.json";
import { estimate, GALLERIES } from "./capacity";
import type { Specimen } from "./App";

const POSITIONS = [
  "Unknown", "Right thumb", "Right index", "Right middle", "Right ring", "Right little",
  "Left thumb", "Left index", "Left middle", "Left ring", "Left little",
];
const STEP_NAMES: Record<string, string> = { decode: "Read image", wsq: "Compress to WSQ", minex: "Extract MINEX" };

const size = (n: number) => (n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n} B`);
const bytes64 = (b64: string) => Math.floor((b64.length * 3) / 4);

type ScreenProps = { specimens: Specimen[]; selected: Specimen | undefined };

function Empty({ children }: { children: React.ReactNode }) {
  return <p className="empty">{children}</p>;
}

function useAction<T>() {
  const [result, setResult] = useState<T>();
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const run = async (action: () => Promise<T>) => {
    setBusy(true);
    setError("");
    setResult(undefined);
    try {
      setResult(await action());
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return { result, error, busy, run };
}

function ErrorLine({ message }: { message: string }) {
  return message ? <p role="alert" className="error">{message}</p> : null;
}

export function ScanScreen({ selected }: ScreenProps) {
  if (!selected) return <Empty>Add a print from the tray to see how it converts.</Empty>;
  const { inspection: ins, name } = selected;
  const stem = name.replace(/\.[^.]+$/, "");
  return (
    <div className="scan">
      <Plate inspection={ins} title={name} />
      <div className="scan-detail">
        <h2>{name}</h2>
        <p className="lede">
          {ins.minutiae.length} minutiae from a {ins.source_format} {ins.kind === "image" ? "image" : "template"}, {ins.width} × {ins.height} px
          {ins.dpi ? ` at ${ins.dpi} dpi` : ""}.
        </p>
        {ins.steps.length > 0 ? (
          <ol className="steps">
            {ins.steps.map((s) => (
              <li key={s.name}>
                <span>{STEP_NAMES[s.name] ?? s.name}</span>
                <span className="num">{size(s.bytes)}</span>
                <span className="num quiet">{s.ms.toFixed(1)} ms</span>
              </li>
            ))}
          </ol>
        ) : (
          <p>This is already a template, so no conversion ran and there is no image to show.</p>
        )}
        <dl className="legend">
          <div><dt><i className="key ending" /></dt><dd>Ridge ending</dd></div>
          <div><dt><i className="key bifurcation" /></dt><dd>Bifurcation</dd></div>
          {ins.minutiae.some((m) => minutiaClass(m.type) === "other") && (
            <div><dt><i className="key other" /></dt><dd>Other</dd></div>
          )}
        </dl>
        <div className="row">
          <button onClick={() => downloadBase64(`${stem}.ansi`, ins.template_ansi)}>Download MINEX</button>
          <button onClick={() => downloadBase64(`${stem}.iso`, ins.template_iso)}>Download ISO</button>
          {ins.wsq && <button onClick={() => downloadBase64(`${stem}.wsq`, ins.wsq!)}>Download WSQ</button>}
        </div>
        <p className="quiet">MINEX {size(bytes64(ins.template_ansi))}, ISO {size(bytes64(ins.template_iso))}</p>
      </div>
    </div>
  );
}

type Picked = Record<string, number>;

function FingerPicker({ specimens, picked, setPicked }: { specimens: Specimen[]; picked: Picked; setPicked: (p: Picked) => void }) {
  return (
    <ul className="picker">
      {specimens.map((s) => {
        const on = s.id in picked;
        return (
          <li key={s.id}>
            <label>
              <input
                type="checkbox"
                checked={on}
                onChange={() => {
                  const next = { ...picked };
                  if (on) delete next[s.id];
                  else next[s.id] = nextFreePosition(picked);
                  setPicked(next);
                }}
              />
              {s.name}
            </label>
            {on && (
              <select
                aria-label={`Finger position for ${s.name}`}
                value={picked[s.id]}
                onChange={(e) => setPicked({ ...picked, [s.id]: Number(e.target.value) })}
              >
                {POSITIONS.map((p, i) => <option key={p} value={i}>{p}</option>)}
              </select>
            )}
          </li>
        );
      })}
    </ul>
  );
}

function nextFreePosition(picked: Picked) {
  const used = new Set(Object.values(picked));
  for (const p of [2, 7, 1, 6, 3, 8, 4, 9, 5, 10]) if (!used.has(p)) return p;
  return 0;
}

const toFingers = (specimens: Specimen[], picked: Picked) =>
  specimens.filter((s) => s.id in picked).map((s) => ({ template: s.inspection.template_ansi, position: picked[s.id] }));

export function EnrolScreen({ specimens, onChanged }: ScreenProps & { onChanged: () => void }) {
  const [id, setId] = useState("");
  const [picked, setPicked] = useState<Picked>({});
  const action = useAction<string>();
  if (specimens.length === 0) return <Empty>Add prints to the tray, then enrol them under a customer ID.</Empty>;
  const count = Object.keys(picked).length;
  return (
    <div className="form-screen">
      <h2>Enrol a customer</h2>
      <label className="field">Customer ID<input value={id} onChange={(e) => setId(e.target.value)} placeholder="customer-1042" /></label>
      <FingerPicker specimens={specimens} picked={picked} setPicked={setPicked} />
      <button
        className="primary"
        disabled={!id.trim() || count === 0 || action.busy}
        onClick={() =>
          action.run(async () => {
            await api.enrol(id.trim(), toFingers(specimens, picked));
            onChanged();
            return `Enrolled ${id.trim()} with ${count} finger${count === 1 ? "" : "s"}.`;
          })
        }
      >
        Enrol customer
      </button>
      <ErrorLine message={action.error} />
      {action.result && <p role="status" className="ok">{action.result}</p>}
    </div>
  );
}

export function IdentifyScreen({ specimens }: ScreenProps) {
  const [picked, setPicked] = useState<Picked>({});
  const action = useAction<IdentifyResult>();
  if (specimens.length === 0) return <Empty>Add a print to the tray to search for its owner.</Empty>;
  const r = action.result;
  return (
    <div className="form-screen">
      <h2>Find who this is</h2>
      <p className="quiet">Choose up to 10 fingers from one person. Two or more make false matches far less likely.</p>
      <FingerPicker specimens={specimens} picked={picked} setPicked={setPicked} />
      <button
        className="primary"
        disabled={Object.keys(picked).length === 0 || Object.keys(picked).length > 10 || action.busy}
        onClick={() => action.run(() => api.identify(toFingers(specimens, picked)))}
      >
        Search gallery
      </button>
      <ErrorLine message={action.error} />
      {r && (
        <div className="verdict-block">
          <p className={r.match ? "verdict yes" : "verdict no"}>{r.match ? `Identified as ${r.id}` : "No match in the gallery"}</p>
          <p className="quiet">Searched {r.compared.toLocaleString()} comparisons in {r.elapsed_ms.toFixed(1)} ms, using {r.fingers_presented} finger{r.fingers_presented === 1 ? "" : "s"}.</p>
          {r.candidates.length > 0 && (
            <table>
              <thead><tr><th>Customer</th><th className="num">Score</th><th className="num">Fingers over threshold</th><th>Decision</th></tr></thead>
              <tbody>
                {r.candidates.map((c) => (
                  <tr key={c.id}><td>{c.id}</td><td className="num">{c.score.toFixed(1)}</td><td className="num">{c.fingers_matched}</td><td>{c.match ? "Match" : "Below threshold"}</td></tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      )}
    </div>
  );
}

function SpecimenSelect({ label, specimens, value, onChange }: { label: string; specimens: Specimen[]; value: string; onChange: (v: string) => void }) {
  return (
    <label className="field">{label}
      <select value={value} onChange={(e) => onChange(e.target.value)}>
        <option value="">Choose a print</option>
        {specimens.map((s) => <option key={s.id} value={s.id}>{s.name}</option>)}
      </select>
    </label>
  );
}

export function CompareScreen({ specimens }: ScreenProps) {
  const [aId, setA] = useState("");
  const [bId, setB] = useState("");
  const action = useAction<CompareResult>();
  const a = specimens.find((s) => s.id === aId);
  const b = specimens.find((s) => s.id === bId);
  if (specimens.length < 2) return <Empty>Add at least two prints to the tray to compare them.</Empty>;
  const r = action.result;
  return (
    <div className="compare-screen">
      <div className="row end">
        <SpecimenSelect label="First print" specimens={specimens} value={aId} onChange={setA} />
        <SpecimenSelect label="Second print" specimens={specimens} value={bId} onChange={setB} />
        <button className="primary" disabled={!a || !b || action.busy} onClick={() => action.run(() => api.compare(a!.file, b!.file))}>Compare prints</button>
      </div>
      <ErrorLine message={action.error} />
      {r && a && b && (
        <>
          <p className={r.match ? "verdict yes" : "verdict no"}>{r.match ? "Same finger" : "Different fingers"}</p>
          <p className="quiet">Score {r.score.toFixed(1)} against a threshold of {r.threshold}, {r.pairs.length} matched minutiae, rotated {r.rotation_deg.toFixed(1)}°.</p>
          <ComparePlates a={a.inspection} b={b.inspection} result={r} />
        </>
      )}
    </div>
  );
}

export function CrossMatchScreen({ specimens }: ScreenProps) {
  const action = useAction<CrossMatch>();
  if (specimens.length < 2) return <Empty>Add at least two prints to the tray to cross-match them.</Empty>;
  const r = action.result;
  return (
    <div className="form-screen wide">
      <h2>Compare every print with every other</h2>
      <button className="primary" disabled={specimens.length > 100 || action.busy} onClick={() => action.run(() => api.crossmatch(specimens.map((s) => ({ name: s.name, file: s.file }))))}>
        Cross-match {specimens.length} prints
      </button>
      <ErrorLine message={action.error} />
      {r && (
        <>
          <p className="quiet">{(r.names.length * (r.names.length + 1)) / 2} comparisons in {r.elapsed_ms.toFixed(1)} ms. Dark cells score at or above {r.threshold}.</p>
          <div className="heat-scroll">
            <table className="heat">
              <thead><tr><th />{r.names.map((n) => <th key={n} scope="col"><span>{n}</span></th>)}</tr></thead>
              <tbody>
                {r.scores.map((row, i) => (
                  <tr key={r.names[i]}>
                    <th scope="row">{r.names[i]}</th>
                    {row.map((score, j) => {
                      const h = heat(score, r.threshold);
                      return (
                        <td key={j} className={h.match ? "hit" : ""} style={{ "--strength": h.strength } as React.CSSProperties} title={`${r.names[i]} and ${r.names[j]}: ${score.toFixed(1)}`}>
                          {score.toFixed(0)}
                        </td>
                      );
                    })}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </div>
  );
}

export function PerformanceScreen({ refresh }: { refresh: number }) {
  const [stats, setStats] = useState<Stats>();
  const [error, setError] = useState("");
  useEffect(() => {
    api.stats().then(setStats).catch((e) => setError(e.message));
  }, [refresh]);
  return (
    <div className="perf">
      <h2>This server now</h2>
      <ErrorLine message={error} />
      {stats && (
        <table>
          <tbody>
            <tr><th>Customers</th><td className="num">{stats.subjects.toLocaleString()}</td></tr>
            <tr><th>Finger templates</th><td className="num">{stats.templates.toLocaleString()}</td></tr>
            <tr><th>Gallery memory</th><td className="num">{stats.memory_mb.toFixed(2)} MB</td></tr>
            <tr><th>Matching threads</th><td className="num">{stats.match_threads}</td></tr>
          </tbody>
        </table>
      )}
      <h2>Search speed</h2>
      <p className="quiet">{benchmarks.machine}. Synthetic templates, one search at a time. Real templates may have more minutiae and run slower.</p>
      <table>
        <thead><tr><th>Gallery</th><th className="num">Average</th><th className="num">Worst</th><th className="num">RAM</th><th>Evidence</th></tr></thead>
        <tbody>
          {benchmarks.rows.map((row) => (
            <tr key={row.label}><td>{row.label}</td><td className="num">{(row.avg_ms / 1000).toFixed(row.avg_ms < 1000 ? 3 : 1)} s</td><td className="num">{(row.worst_ms / 1000).toFixed(row.worst_ms < 1000 ? 3 : 1)} s</td><td className="num">{(row.memory_mb / 1024).toFixed(1)} GB</td><td>{row.evidence}</td></tr>
          ))}
        </tbody>
      </table>
      <h2>Estimated hardware</h2>
      <p className="quiet">Projected from the slowest measured run, one search at a time on today's engine. RAM is for one copy; multiply by replicas. Stage 2 cores assume the planned pre-filter, which is not built yet, and exclude Stage 1.</p>
      <div className="heat-scroll">
        <table>
          <thead>
            <tr><th>Templates</th><th className="num">RAM</th><th className="num">Search, 16 cores</th><th className="num">32 cores</th><th className="num">64 cores</th><th className="num">Cores per search/s</th><th className="num">Stage 2 cores for 1,000/s, 99.9% filter</th></tr>
          </thead>
          <tbody>
            {GALLERIES.map(estimate).map((e) => (
              <tr key={e.templates}>
                <td>{e.templates / 1e6}M</td>
                <td className="num">{e.ramGb[0].toFixed(0)}-{e.ramGb[1].toFixed(0)} GB</td>
                <td className="num">{e.searchS.c16.toFixed(1)} s</td>
                <td className="num">{e.searchS.c32.toFixed(1)} s</td>
                <td className="num">{e.searchS.c64.toFixed(1)} s</td>
                <td className="num">{e.coresPerSearchPerS.toLocaleString()}</td>
                <td className="num">{e.stage2At999.toLocaleString()}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="note">Accuracy has not yet been measured on real scanner data at this scale. Scaling to 10M templates and 1,000 searches per second is a design target, not a measured result.</p>
    </div>
  );
}
