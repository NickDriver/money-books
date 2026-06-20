#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../support/mb_compat.h"   /* mkdir/stat/S_ISDIR/unlink (portable across POSIX/Win32) */
#include "vendor/cjson/cJSON.h"

/* ---- config dir (the one platform-specific bit; D23) ---- */
#if defined(__APPLE__)
#  define MB_APP_SUBDIR "/Library/Application Support/MoneyBooks"
#else
#  define MB_APP_SUBDIR "/.local/share/MoneyBooks"   /* XDG-ish fallback */
#endif

static mb_err app_dir(char *buf, size_t n) {
  const char *home = getenv("MB_APP_DIR");   /* test/override hook */
  if (home && home[0]) {
    snprintf(buf, n, "%s", home);
  } else {
    home = getenv("HOME");
    if (!home || !home[0]) return MB_FAIL(MB_ERR_IO, "HOME not set");
    snprintf(buf, n, "%s%s", home, MB_APP_SUBDIR);
  }
  if (mkdir(buf, 0700) != 0) {
    /* EEXIST is fine; anything else is a real error */
    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode))
      return MB_FAIL(MB_ERR_IO, "cannot create app dir '%s'", buf);
  }
  return MB_OK;
}

mb_err mb_registry_books_dir(char *buf, size_t n) { return app_dir(buf, n); }

mb_err mb_registry_default_path(char *buf, size_t n) {
  char dir[1024];
  MB_TRY(app_dir(dir, sizeof dir));
  snprintf(buf, n, "%s/books.json", dir);
  return MB_OK;
}

/* Read the registry file into a cJSON array (empty array if the file is absent). */
static cJSON *read_all(const char *regpath) {
  FILE *f = fopen(regpath, "rb");
  if (!f) return cJSON_CreateArray();
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  if (len < 0) { fclose(f); return cJSON_CreateArray(); }
  fseek(f, 0, SEEK_SET);
  char *txt = malloc((size_t)len + 1);
  if (!txt) { fclose(f); return cJSON_CreateArray(); }
  size_t rd = fread(txt, 1, (size_t)len, f);
  txt[rd] = '\0';
  fclose(f);
  cJSON *arr = cJSON_Parse(txt);
  free(txt);
  if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return cJSON_CreateArray(); }
  return arr;
}

static mb_err write_all(const char *regpath, cJSON *arr) {
  char *txt = cJSON_PrintUnformatted(arr);
  if (!txt) return MB_FAIL(MB_ERR_INTERNAL, "json print failed");
  FILE *f = fopen(regpath, "wb");
  if (!f) { free(txt); return MB_FAIL(MB_ERR_IO, "cannot write '%s'", regpath); }
  size_t len = strlen(txt);
  size_t wr = fwrite(txt, 1, len, f);
  int ok = (fclose(f) == 0) && (wr == len);
  free(txt);
  return ok ? MB_OK : MB_FAIL(MB_ERR_IO, "short write to '%s'", regpath);
}

static cJSON *find_by_path(cJSON *arr, const char *bookpath) {
  cJSON *o;
  cJSON_ArrayForEach(o, arr) {
    cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "path");
    if (cJSON_IsString(p) && !strcmp(p->valuestring, bookpath)) return o;
  }
  return NULL;
}

mb_err mb_registry_touch(const char *regpath, const char *bookpath,
                         const char *name, long long now) {
  if (!bookpath || !bookpath[0]) return MB_FAIL(MB_ERR_INVALID_ARG, "bookpath required");
  cJSON *arr = read_all(regpath);
  cJSON *o = find_by_path(arr, bookpath);
  if (!o) {
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", bookpath);
    cJSON_AddStringToObject(o, "name", name ? name : bookpath);
    cJSON_AddNumberToObject(o, "last_opened", (double)now);
    cJSON_AddItemToArray(arr, o);
  } else {
    if (name) cJSON_ReplaceItemInObjectCaseSensitive(o, "name", cJSON_CreateString(name));
    cJSON_ReplaceItemInObjectCaseSensitive(o, "last_opened", cJSON_CreateNumber((double)now));
  }
  mb_err e = write_all(regpath, arr);
  cJSON_Delete(arr);
  return e;
}

mb_err mb_registry_forget(const char *regpath, const char *bookpath) {
  cJSON *arr = read_all(regpath);
  int idx = 0, found = -1;
  cJSON *o;
  cJSON_ArrayForEach(o, arr) {
    cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "path");
    if (cJSON_IsString(p) && bookpath && !strcmp(p->valuestring, bookpath)) { found = idx; break; }
    idx++;
  }
  if (found >= 0) cJSON_DeleteItemFromArray(arr, found);
  mb_err e = write_all(regpath, arr);
  cJSON_Delete(arr);
  return e;
}

static int cmp_recent(const void *a, const void *b) {
  long long la = ((const mb_book_ref *)a)->last_opened;
  long long lb = ((const mb_book_ref *)b)->last_opened;
  return (la < lb) - (la > lb);   /* descending */
}

mb_err mb_registry_list(const char *regpath, mb_book_ref **out, int *n) {
  cJSON *arr = read_all(regpath);
  int count = cJSON_GetArraySize(arr);
  mb_book_ref *rows = calloc((size_t)(count > 0 ? count : 1), sizeof *rows);
  if (!rows) { cJSON_Delete(arr); return MB_FAIL(MB_ERR_INTERNAL, "oom"); }
  int cnt = 0;
  cJSON *o;
  cJSON_ArrayForEach(o, arr) {
    cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "path");
    cJSON *nm = cJSON_GetObjectItemCaseSensitive(o, "name");
    cJSON *lo = cJSON_GetObjectItemCaseSensitive(o, "last_opened");
    if (!cJSON_IsString(p)) continue;
    mb_book_ref *r = &rows[cnt++];
    snprintf(r->path, sizeof r->path, "%s", p->valuestring);
    snprintf(r->name, sizeof r->name, "%s", cJSON_IsString(nm) ? nm->valuestring : p->valuestring);
    r->last_opened = cJSON_IsNumber(lo) ? (long long)lo->valuedouble : 0;
  }
  cJSON_Delete(arr);
  if (cnt > 1) qsort(rows, (size_t)cnt, sizeof *rows, cmp_recent);
  *out = rows;
  *n = cnt;
  return MB_OK;
}

#ifdef MB_TEST
#include "../support/mb_test.h"

TEST(registry, add_update_list_forget) {
  const char *reg = "/tmp/mb_registry_test.json";
  unlink(reg);

  ASSERT_OK(mb_registry_touch(reg, "/tmp/acme.sqlite", "Acme LLC", 100));
  ASSERT_OK(mb_registry_touch(reg, "/tmp/beta.sqlite", "Beta Co", 200));

  mb_book_ref *rows = NULL; int n = 0;
  ASSERT_OK(mb_registry_list(reg, &rows, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_STR_EQ(rows[0].name, "Beta Co");      /* most-recent first */
  ASSERT_STR_EQ(rows[1].name, "Acme LLC");
  free(rows);

  /* re-touch Acme moves it to the front and can rename it */
  ASSERT_OK(mb_registry_touch(reg, "/tmp/acme.sqlite", "Acme Inc", 300));
  ASSERT_OK(mb_registry_list(reg, &rows, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_STR_EQ(rows[0].name, "Acme Inc");
  free(rows);

  ASSERT_OK(mb_registry_forget(reg, "/tmp/acme.sqlite"));
  ASSERT_OK(mb_registry_list(reg, &rows, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_STR_EQ(rows[0].name, "Beta Co");
  free(rows);
  unlink(reg);
}
#endif
