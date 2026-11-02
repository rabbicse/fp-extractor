// Hardware estimates from measured M1 benchmarks. Change the constants when new measurements exist.
export const COMPARISONS_PER_CORE_PER_S = 16_250; // slowest measured run: 130k/s on 8 threads
export const RAM_GB_PER_MILLION = 1.54; // measured: 7,715 MB at 5M synthetic templates
export const REAL_TEMPLATE_RAM_FACTOR = 2; // real templates can hold about twice the minutiae
export const WSQ_KB_RANGE = [10, 30]; // estimate until measured per scanner
export const TEMPLATE_KB = 1.5; // upper bound per stored template

export const searchSeconds = (templates: number, cores: number) =>
  templates / (cores * COMPARISONS_PER_CORE_PER_S);

export const coresForRate = (templates: number, perSecond: number, keepFraction = 1) =>
  Math.ceil((templates * keepFraction * perSecond) / COMPARISONS_PER_CORE_PER_S);

export function estimate(templates: number) {
  const millions = templates / 1e6;
  const ramLow = millions * RAM_GB_PER_MILLION;
  return {
    templates,
    ramGb: [ramLow, ramLow * REAL_TEMPLATE_RAM_FACTOR],
    searchS: { c16: searchSeconds(templates, 16), c32: searchSeconds(templates, 32), c64: searchSeconds(templates, 64) },
    coresPerSearchPerS: coresForRate(templates, 1),
    fullScan1000: coresForRate(templates, 1000),
    stage2At99: coresForRate(templates, 1000, 0.01),
    stage2At999: coresForRate(templates, 1000, 0.001),
    templateDiskGb: millions * TEMPLATE_KB,
    wsqGb: WSQ_KB_RANGE.map((kb) => millions * kb),
  };
}

export const GALLERIES = [1e6, 2e6, 5e6, 10e6, 20e6, 50e6, 100e6];
