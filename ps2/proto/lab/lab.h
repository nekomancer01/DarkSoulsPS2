/* lab.h: the Lab (Polaris 1.7b): the simulate phase as a 32-bit host program (lab/README.md) */
#ifndef LAB_H
#define LAB_H
extern const char *lab_root;  /* the build directory (its data/ holds the packs) */
extern int lab_noenemies;  /* the script's "option noenemies": the console places no enemies either */
extern const char *lab_area;  /* the booted area pack ("m10_02_00_00") */
const char *lab_path(const char *path, char *out, unsigned cap);
/* a stub was called: counted per name, printed by "stubs" in two lists (Polaris 1.7c):
 *   LAB_COUNT     presentation: render, sound, UI; the simulation never reads its result (expected, fine to ignore)
 *   LAB_READBACK  the simulation reads the result back (a value returned, or state simulate tests later): the real
 *                 layer violations of the Polaris 0.4 boundary map, and a script reaching one fails in the Lab */
void lab_count(const char *name, int readback);
#define LAB_COUNT(name) lab_count(name, 0)
#define LAB_READBACK(name) lab_count(name, 1)
/* a test-script command the Lab cannot run (lab_stubs.c): "TEST error line N: ..." with the command named */
void lab_unsupported(const char *cmd, const char *needs);
#endif
