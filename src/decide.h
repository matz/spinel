#ifndef SPINEL_DECIDE_H
#define SPINEL_DECIDE_H
#include "node_table.h"

/* The decision registry: one question for every optimization that would be a
   miscompile if its legality check were wrong. See decide.c. */

/* 0 unless SPINEL_DECISIONS or SPINEL_DECISIONS_LOG is set. */
extern int g_decide_on;

/* Read the two environment variables. Returns g_decide_on; leaves the process
   when the allow-list cannot be read or the log cannot be written. */
int decide_setup(void);
/* Write the log, if one was asked for. */
void decide_write_log(void);

/* The site of a decision about a function emitted from node `id` (a proc or
   fiber body): its position, for decide_fn. "" while the registry is off. */
const char *decide_node_site(const NodeTable *nt, int id);

int decide_node_keyed(const NodeTable *nt, int id, const char *kind, const char *name);
int decide_fn_keyed(const char *kind, const char *site, const char *name);

/* May the decision `kind` at node `id` be taken? Ask once the optimization's
   own legality check has passed; 0 means take the conservative path. `name`
   tells apart two decisions of one kind at one position, or is NULL. */
static inline int decide_node(const NodeTable *nt, int id, const char *kind, const char *name) {
  return g_decide_on ? decide_node_keyed(nt, id, kind, name) : 1;
}
/* The same for a decision about a whole function: `site` names it by its
   class and method (Class#meth), never by an emitted C name. */
static inline int decide_fn(const char *kind, const char *site, const char *name) {
  return g_decide_on ? decide_fn_keyed(kind, site, name) : 1;
}

#endif
