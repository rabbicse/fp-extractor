import type { CompareResult, Inspection } from "./api";
import { minutiaClass, pairLine, tailEnd, toImagePixels } from "./geometry";

function Markers({ inspection, offset = 0 }: { inspection: Inspection; offset?: number }) {
  return (
    <g transform={`translate(${offset} 0)`}>
      {inspection.minutiae.map((m, i) => {
        const end = tailEnd(m, 13);
        return (
          <g key={i} className={`marker ${minutiaClass(m.type)}`}>
            {m.type === 2 ? (
              <rect x={m.x - 3.5} y={m.y - 3.5} width="7" height="7" />
            ) : (
              <circle cx={m.x} cy={m.y} r="4" />
            )}
            <line x1={m.x} y1={m.y} x2={end.x} y2={end.y} />
          </g>
        );
      })}
    </g>
  );
}

function PrintImage({ inspection, offset = 0 }: { inspection: Inspection; offset?: number }) {
  if (!inspection.preview_bmp)
    return <rect x={offset} width={inspection.width} height={inspection.height} className="template-ground" />;
  return (
    <image
      x={offset}
      width={inspection.width}
      height={inspection.height}
      href={`data:image/bmp;base64,${inspection.preview_bmp}`}
    />
  );
}

export function Plate({ inspection, title }: { inspection: Inspection; title: string }) {
  return (
    <svg
      className="plate"
      role="img"
      aria-label={`${title}, ${inspection.minutiae.length} minutiae`}
      viewBox={`0 0 ${inspection.width} ${inspection.height}`}
    >
      <PrintImage inspection={inspection} />
      <Markers inspection={inspection} />
    </svg>
  );
}

const GAP = 56;

export function ComparePlates({ a, b, result }: { a: Inspection; b: Inspection; result: CompareResult }) {
  const dpiA = a.dpi ?? 500;
  const dpiB = b.dpi ?? 500;
  return (
    <svg
      className="plate compare"
      role="img"
      aria-label={`${result.pairs.length} matched minutiae between the two prints`}
      viewBox={`0 0 ${a.width + GAP + b.width} ${Math.max(a.height, b.height)}`}
    >
      <PrintImage inspection={a} />
      <PrintImage inspection={b} offset={a.width + GAP} />
      {result.pairs.map((pair, i) => {
        const line = pairLine(toImagePixels(pair.a, dpiA), toImagePixels(pair.b, dpiB), a.width, GAP);
        return <line key={i} className="pair" style={{ animationDelay: `${i * 18}ms` }} {...line} />;
      })}
    </svg>
  );
}
