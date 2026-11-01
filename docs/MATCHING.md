# Minutiae matching ("MINEX matching")

FingerJet produces ANSI-378 / ISO-19794-2 templates but has **no matcher**.
MINEX itself is a NIST *test programme* for template interoperability, not
an algorithm, so "MINEX matching" means: compare two such templates and get a
similarity score. `fpx::Matcher` does that.

## Algorithm

Input: two templates (any resolution, ANSI or ISO, any vendor).

1. **Compile** (once per template, cached in the gallery)
   * normalise coordinates to 500 dpi (197 px/cm) and angles to 1/256 turn;
   * keep at most 128 minutiae (highest quality first);
   * for each minutia *i* build local structures to its *k = 6* nearest
     neighbours *j*: edge length `d`, direction of the edge relative to the
     minutia direction `a = angle(i→j) − θi`, and relative neighbour direction
     `b = θj − θi`. All three are invariant to rotation and translation.
2. **Index the probe** (once per search): every probe edge is registered in
   every cell of a 64 × 16 × 16 grid over `(d, a, b)` that its tolerance box
   overlaps. A candidate edge then needs a single O(1) bucket look-up.
3. **Local correspondences**: scan the candidate's edges, look them up, and
   keep pairs within tolerance (`±6 px + 5 %` length, `±14°` angles). Each
   correspondence implies a global rotation `θp − θc`.
4. **Rotation consensus**: histogram of implied rotations (32 bins); keep the
   correspondences near the peak; circular mean = global rotation.
5. **Seeds**: minutia pairs with the most consistent correspondences. If no
   pair has ≥ 3 the comparison ends here with score 0 (*exact* early exit,
   see step 7); most impostor comparisons stop here.
6. **Alignment**: each seed defines a rigid transform; a cheap count picks
   the best seed; minutiae are then paired greedily (closest first) within a
   radius of 14 px growing by 6 % of the distance from the seed (absorbs skin
   distortion) and ±25° direction. The transform is refined by least squares
   and pairing is repeated.
7. **Graph consistency**: a paired minutia counts only if at least 3 of its
   local edges lead to neighbours that are *also* paired with the
   corresponding candidate neighbour. Random alignments of impostors pair
   minutiae by chance, but those pairs are almost never connected by matching
   edges. This is what keeps the impostor tail low enough for 1:N at 10⁶.
8. **Score** = `100 · m² / (max(n_p, 20) · max(n_c, 20))`, where `m` is the
   number of consistent pairs and `n_p`, `n_c` are the minutiae inside the
   overlapping area of probe and candidate. The floor of 20 prevents tiny
   overlaps with 4–5 chance pairs from scoring high.

1:1 `Matcher::match(a, b)` runs both directions and averages, so the score
does not depend on argument order. 1:N runs probe → candidate only.

All per-comparison work uses fixed-size stack buffers: no heap allocation,
thread-safe. Measured: ~19 µs per comparison per core on the 4-vCPU
development container (≈ 52 k comparisons/s/core, 210 k/s on 4 cores).

## Calibration

Thresholds (`include/fpx/matcher.hpp`):

| Constant | Value | Use |
|---|---|---|
| `kVerifyThreshold` | 12 | 1:1 verification |
| `kIdentifyThreshold` | 18 | 1:N identification against ~10⁶ templates |

Where these numbers come from (synthetic data, reproducible with the CLI):

| Experiment | Genuine scores | Impostor scores |
|---|---|---|
| `fpx eval` on 80 synthetic fingers × 4 impressions, full image pipeline (synthesis → FingerJet), 480 genuine / 3160 impostor pairs | min 42.7, mean 72.5 | max 3.9, mean 0.25 |
| `fpx bench --gallery 1000000 --probes 40` template-level (rotation ±25°, shift ±40 px, 25 % missed, 15 % spurious minutiae), 10 M impostor comparisons | 39/40 found at rank 1 above 18 (those: min 24.5, mean 45.8) | best of 1 M per probe: max 16.0 |

Self-comparisons score 95–100, not always exactly 100: minutiae with no
well-connected neighbours are excluded by the graph-consistency rule.

The 1:N margin (16.0 vs 18) is thin with a single finger. Treat
`kIdentifyThreshold` as a starting point and use two fingers per customer for
million-scale identification.

**Synthetic data is easier than real data.** Real fingers have more
distortion, partial contact, dry/wet skin and scars; real impostors of the
same pattern class are more alike. Before production:

1. Collect ≥ 100 fingers × 4–8 impressions from *your* scanners (or use
   FVC2002/2004 DB1–DB4 for a first impression), named
   `<finger>_<impression>.bmp`.
2. `fpx eval /path/to/folder -v` → EER, FNMR at FMR 1 % / 0.1 %, hardest
   genuine and impostor pairs.
3. For 1:N, pick the identify threshold so that
   `gallery_size × FMR(threshold)` (expected false hits per search) is well
   below 1, e.g. FMR ≤ 10⁻⁸ for 10⁶ templates. Estimate the impostor tail
   with many impostor comparisons (`fpx eval` on a large folder, or
   identify non-enrolled fingers against your production gallery size).
4. Tune with `--matcher "key=value,..."` (all `MatcherConfig` fields), e.g.
   `fpx eval data/ --matcher "pair_dist_tol=18,min_pair_support=2"`, then
   bake the result into `MatcherConfig` defaults or pass it to `Gallery`.

## Improving accuracy further (ideas, in order of value)

* **Enrol several fingers / impressions per customer** and fuse scores
  (max or sum). Two fingers reduce 1:N false matches by orders of magnitude
  and are the standard way large AFIS systems reach low FPIR.
* Quality gate at capture: reject images with < 12 minutiae or low contrast,
  ask for a re-scan.
* Store 2–3 enrolment impressions per finger (template fusion or best-of).
* Add ridge-count / orientation-field features (needs extended data or the
  image), or switch to a descriptor approach such as Minutia Cylinder-Code
  (check its patent/licence status for commercial use).
* Pattern-class or orientation-field pre-filtering to cut 1:N work by 3–10×.
