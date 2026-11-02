import { useRef, useState } from "react";
import { api, fileToBase64 } from "./api";
import type { Inspection } from "./api";
import { CompareScreen, CrossMatchScreen, EnrolScreen, IdentifyScreen, PerformanceScreen, ScanScreen } from "./screens";

export type Specimen = { id: string; name: string; file: string; inspection: Inspection; finger?: number; impression?: number };

const TABS = ["Scan", "Enrol", "Identify", "Compare", "Cross-match", "Performance"] as const;
type Tab = (typeof TABS)[number];

export function App() {
  const [specimens, setSpecimens] = useState<Specimen[]>([]);
  const [selectedId, setSelectedId] = useState("");
  const [tab, setTab] = useState<Tab>("Scan");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [refresh, setRefresh] = useState(0);
  const nextFinger = useRef(1);
  const input = useRef<HTMLInputElement>(null);

  const add = (added: Specimen[]) => {
    setSpecimens((list) => [...list, ...added]);
    if (added[0]) setSelectedId(added[0].id);
  };

  const guard = async (work: () => Promise<void>) => {
    setBusy(true);
    setError("");
    try {
      await work();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  const generate = async (finger: number, impression: number): Promise<Specimen> => {
    const { file } = await api.synthetic(finger, impression);
    const inspection = await api.inspect(file);
    return { id: crypto.randomUUID(), name: `mock-${finger}-${impression + 1}.bmp`, file, inspection, finger, impression };
  };

  const generateNew = () => guard(async () => add([await generate(nextFinger.current++, 0)]));

  const selected = specimens.find((s) => s.id === selectedId);

  const generateImpression = () =>
    guard(async () => {
      if (!selected?.finger) return;
      const taken = specimens.filter((s) => s.finger === selected.finger).map((s) => s.impression ?? 0);
      add([await generate(selected.finger, Math.max(...taken) + 1)]);
    });

  const upload = (files: FileList | null) =>
    guard(async () => {
      const failed: string[] = [];
      const added: Specimen[] = [];
      for (const f of Array.from(files ?? [])) {
        try {
          const file = await fileToBase64(f);
          added.push({ id: crypto.randomUUID(), name: f.name, file, inspection: await api.inspect(file) });
        } catch (e) {
          failed.push(`${f.name}: ${(e as Error).message}`);
        }
      }
      add(added);
      if (input.current) input.current.value = "";
      if (failed.length) throw new Error(failed.join(" "));
    });

  const loadSample = () =>
    guard(async () => {
      const added: Specimen[] = [];
      for (let person = 0; person < 6; person++) {
        const id = `customer-${1001 + person}`;
        const fingers = [];
        for (const position of [2, 7]) {
          const finger = 1000 + person * 16 + position;
          const { file } = await api.synthetic(finger, 0);
          const inspection = await api.inspect(file);
          fingers.push({ template: inspection.template_ansi, position });
        }
        await api.enrol(id, fingers);
      }
      for (const position of [2, 7]) added.push(await generate(1000 + position, 1));
      added.push(await generate(9000, 0));
      add(added);
      setRefresh((r) => r + 1);
    });

  return (
    <div className="shell">
      <aside className="tray" aria-label="Print tray">
        <h1>fp-extractor</h1>
        <p className="banner">Demo only. Use mock data or your own test scans, not other people's fingerprints.</p>
        <div className="tray-actions">
          <button className="primary" disabled={busy} onClick={loadSample}>Load sample gallery</button>
          <button disabled={busy} onClick={generateNew}>Generate a finger</button>
          <button disabled={busy || !selected?.finger} onClick={generateImpression}>Rescan selected finger</button>
          <button disabled={busy} onClick={() => input.current?.click()}>Upload files</button>
          <input ref={input} data-testid="upload" type="file" multiple hidden accept=".bmp,.wsq,.pgm,.tif,.tiff,.ansi,.iso,.minex,.fmr" onChange={(e) => upload(e.target.files)} />
        </div>
        <p className="quiet">BMP, WSQ, MINEX or ISO</p>
        {busy && <p role="status" className="quiet">Working</p>}
        {error && <p role="alert" className="error">{error}</p>}
        <ul className="specimens">
          {specimens.map((s) => (
            <li key={s.id}>
              <button aria-pressed={s.id === selectedId} onClick={() => { setSelectedId(s.id); setTab("Scan"); }}>
                <span>{s.name}</span>
                <span className="num quiet">{s.inspection.minutiae.length}</span>
              </button>
            </li>
          ))}
        </ul>
        {specimens.length === 0 && <p className="quiet">The tray is empty. Load the sample gallery to start.</p>}
      </aside>
      <main>
        <nav aria-label="Screens">
          {TABS.map((t) => (
            <button key={t} aria-current={t === tab ? "page" : undefined} onClick={() => setTab(t)}>{t}</button>
          ))}
        </nav>
        <section>
          {tab === "Scan" && <ScanScreen specimens={specimens} selected={selected} />}
          {tab === "Enrol" && <EnrolScreen specimens={specimens} selected={selected} onChanged={() => setRefresh((r) => r + 1)} />}
          {tab === "Identify" && <IdentifyScreen specimens={specimens} selected={selected} />}
          {tab === "Compare" && <CompareScreen specimens={specimens} selected={selected} />}
          {tab === "Cross-match" && <CrossMatchScreen specimens={specimens} selected={selected} />}
          {tab === "Performance" && <PerformanceScreen refresh={refresh} />}
        </section>
      </main>
    </div>
  );
}
