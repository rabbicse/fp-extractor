# NBIS WSQ subset

The files under `src/` and `include/` are an unmodified subset of
[NIST Biometric Image Software (NBIS)](https://www.nist.gov/services-resources/software/nist-biometric-image-software-nbis)
release 5.0.0 (via the mirror https://github.com/lessandro/nbis).

Only what is needed to encode/decode FBI WSQ images is included:

| Directory in NBIS | Files |
|---|---|
| `imgtools/src/lib/wsq` | all |
| `imgtools/src/lib/jpegl` | `huff.c`, `tableio.c`, `huftable.c`, `util.c` (renamed with `jpegl_` prefix) |
| `commonnbis/src/lib/ioutil` | `dataio.c`, `filesize.c` |
| `commonnbis/src/lib/util` | `computil.c`, `syserr.c`, `fatalerr.c` |
| `commonnbis/src/lib/fet` | all except `printfet.c` |


`nbis_globals.c` is ours: it defines the `debug` flag NBIS programs normally provide.

**License:** NBIS was developed by NIST employees and is in the public domain
(17 U.S.C. §105). See the header of each source file.

**Thread safety:** the NBIS WSQ codec uses global tables. The C++ wrapper
(`src/core/wsq.cpp`) serialises all calls with a mutex.
