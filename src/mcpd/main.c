/*
 * Money Books — stdio MCP server (Phase 5). The binary an MCP client (e.g. Claude Desktop)
 * launches. Reads newline-delimited JSON-RPC from stdin, writes responses to stdout, logs to
 * stderr. Per SPEC D8 this is the proxy/entry that drives the single-writer engine.
 *
 * Built by `make mcp` (pure C, no webview). Not part of the test/library build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store/store.h"
#include "account/account.h"
#include "seed/seed.h"
#include "mcp/mcp.h"

int main(int argc, char **argv) {
  const char *db = (argc > 1) ? argv[1] : "money.sqlite";

  mb_store *s = NULL;
  if (mb_store_open(db, &s) != MB_OK) {
    fprintf(stderr, "money-books-mcp: cannot open '%s': %s\n", db, mb_last_error()->message);
    return 1;
  }
  /* first run: seed the starter chart so AR/AP/tax accounts exist */
  int n = 0;
  if (mb_account_count(s, &n) == MB_OK && n == 0) {
    if (mb_seed_starter_chart(s) != MB_OK)
      fprintf(stderr, "money-books-mcp: seed warning: %s\n", mb_last_error()->message);
  }
  fprintf(stderr, "money-books-mcp: serving '%s' over stdio\n", db);

  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) != -1) {
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0] == '\0') continue;            /* skip blank lines */
    char *out = NULL;
    if (mb_mcp_handle(s, line, &out) != MB_OK) {
      fprintf(stderr, "money-books-mcp: handler error: %s\n", mb_last_error()->message);
    }
    if (out) {                                 /* notifications produce no reply */
      fputs(out, stdout);
      fputc('\n', stdout);
      fflush(stdout);
      free(out);
    }
  }
  free(line);
  mb_store_close(s);
  return 0;
}
