#ifndef LILJACK_C_GIT_DAG_H
#define LILJACK_C_GIT_DAG_H
#include <json-c/json.h>
/* Commit DAG for the Git tile.
 *
 * Takes the lane-assigned rows from review_panel.git_dag — graph_data supplies
 * commits and parents, the lane assignment is done before we get here — and
 * draws rails, commit marks and subjects through the shared primitives, so the
 * same code serves the window and the terminal.
 *
 * Returns full content height in pixels for the caller's scrollbar.
 *
 * ⚠ Truncation is DRAWN, not hidden: a history cut at the limit says so, because
 * a graph that silently stops looks like a repository with no older history. */
int lj_git_dag_draw(json_object *dag, int x, int y, int w, int h, int scroll, int ansi);
#endif
