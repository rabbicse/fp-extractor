// Pure drawing maths for plates and the heatmap, kept separate so it can be unit tested.
export type Point = { x: number; y: number };
export type Minutia = Point & { angle: number; type: number; quality: number };

/** End of a minutia's direction tail. Angle is 1/256 turn, counter-clockwise; screen y grows downward. */
export function tailEnd(m: Minutia, length: number): Point {
  const radians = (m.angle / 256) * 2 * Math.PI;
  return { x: m.x + Math.cos(radians) * length, y: m.y - Math.sin(radians) * length };
}

/** The server reports matched pairs in 500 dpi units; plates draw in image pixels. */
export function toImagePixels(p: Point, dpi: number): Point {
  const scale = dpi / 500;
  return { x: p.x * scale, y: p.y * scale };
}

/** A line from a point on the left plate to a point on the right plate, offset by the plate gap. */
export function pairLine(a: Point, b: Point, leftWidth: number, gap: number) {
  return { x1: a.x, y1: a.y, x2: b.x + leftWidth + gap, y2: b.y };
}

export function minutiaClass(type: number): "ending" | "bifurcation" | "other" {
  return type === 1 ? "ending" : type === 2 ? "bifurcation" : "other";
}

/** Heatmap cell strength from 0 to 1. At or above the threshold a cell counts as a match. */
export function heat(score: number, threshold: number): { strength: number; match: boolean } {
  const match = score >= threshold;
  if (match) return { strength: Math.min(1, 0.45 + (score / 100) * 0.55), match };
  return { strength: threshold > 0 ? Math.max(0, score / threshold) * 0.3 : 0, match };
}
