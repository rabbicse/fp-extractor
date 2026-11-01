/* NBIS references a global `debug` flag normally defined by its command-line
 * programs (globals.c only has a tentative definition, which a static library
 * does not export reliably). Defined here so the library links stand-alone. */
int debug = 0;
