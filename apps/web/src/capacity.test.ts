import { describe, expect, it } from "vitest";
import { coresForRate, estimate, searchSeconds } from "./capacity";

describe("capacity estimates", () => {
  it("search time falls with cores", () => {
    expect(searchSeconds(10e6, 64)).toBeCloseTo(9.6, 1);
    expect(searchSeconds(10e6, 16)).toBeCloseTo(38.5, 1);
  });
  it("cores scale with rate and rounds up", () => {
    expect(coresForRate(10e6, 1)).toBe(616);
    expect(coresForRate(10e6, 1000)).toBe(615385);
  });
  it("99.9% filtering needs a tenth of the Stage 2 cores of 99%", () => {
    const e = estimate(10e6);
    expect(e.stage2At999).toBe(616);
    expect(e.stage2At99).toBe(6154);
  });
  it("RAM range allows for larger real templates", () => {
    const [low, high] = estimate(10e6).ramGb;
    expect(low).toBeCloseTo(15.4);
    expect(high).toBeCloseTo(30.8);
  });
});
