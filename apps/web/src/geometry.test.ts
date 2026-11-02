import { describe, expect, it } from "vitest";
import { heat, minutiaClass, pairLine, tailEnd, toImagePixels } from "./geometry";

const at = (angle: number) => ({ x: 100, y: 100, angle, type: 1, quality: 80 });

describe("tailEnd", () => {
  it("points right at angle 0", () => {
    const e = tailEnd(at(0), 10);
    expect(e.x).toBeCloseTo(110);
    expect(e.y).toBeCloseTo(100);
  });
  it("points up the screen at a quarter turn, because angles run counter-clockwise", () => {
    const e = tailEnd(at(64), 10);
    expect(e.x).toBeCloseTo(100);
    expect(e.y).toBeCloseTo(90);
  });
});

describe("toImagePixels", () => {
  it("leaves 500 dpi unchanged", () => expect(toImagePixels({ x: 40, y: 8 }, 500)).toEqual({ x: 40, y: 8 }));
  it("doubles at 1000 dpi", () => expect(toImagePixels({ x: 40, y: 8 }, 1000)).toEqual({ x: 80, y: 16 }));
});

describe("pairLine", () => {
  it("offsets only the right end by plate width and gap", () => {
    expect(pairLine({ x: 1, y: 2 }, { x: 3, y: 4 }, 320, 40)).toEqual({ x1: 1, y1: 2, x2: 363, y2: 4 });
  });
});

describe("minutiaClass", () => {
  it("maps template types", () => {
    expect([1, 2, 0].map(minutiaClass)).toEqual(["ending", "bifurcation", "other"]);
  });
});

describe("heat", () => {
  it("treats the threshold itself as a match", () => expect(heat(12, 12).match).toBe(true));
  it("keeps non-matches visibly weaker than the weakest match", () => {
    expect(heat(11.9, 12).strength).toBeLessThan(heat(12, 12).strength);
  });
  it("stays within 0 to 1", () => {
    expect(heat(100, 12).strength).toBeLessThanOrEqual(1);
    expect(heat(0, 12).strength).toBe(0);
    expect(heat(0, 0).match).toBe(true);
  });
});
