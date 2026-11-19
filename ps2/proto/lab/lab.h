/* lab.h: the Lab (Polaris 1.7b): the simulate phase as a 32-bit host program (lab/README.md) */
#ifndef LAB_H
#define LAB_H
extern const char *lab_root;  /* the build directory (its data/ holds the packs) */
const char *lab_path(const char *path, char *out, unsigned cap);
/* a stub was called: counted per name, printed by "stubs" (a simulate module reaching a render / sound / IOP call is
 * a layer violation of the Polaris 0.4 boundary map: listed, not hidden) */
void lab_count(const char *name);
#define LAB_COUNT(name) lab_count(name)
#endif
