/*
 * SQLite, vendored and compiled as its own translation unit.
 *
 * The whole project links this instead of a system libsqlite3, so there is no
 * external SQLite dependency anywhere (Windows ships none; macOS/Linux versions
 * drift). Compile-time options are baked in *here* — not in the build flags — so
 * every build system (Make, CMake) and every platform gets byte-identical SQLite
 * behavior. Our strict -Werror/-Wpedantic warnings are silenced for this
 * third-party code, and the build compiles it once into its own object (no
 * sanitizers, -O2) since it is excluded from the normal source glob.
 *
 * To update SQLite: replace src/vendor/sqlite/{sqlite3.c,sqlite3.h,sqlite3ext.h}
 * and keep src/vendor/sqlite/NOTES.txt current (version + verified SHA3-256).
 */
#define SQLITE_ENABLE_FTS5           1  /* declared in the project's original build flags */
#define SQLITE_DEFAULT_FOREIGN_KEYS  1  /* belt-and-suspenders with PRAGMA foreign_keys=ON */
#define SQLITE_THREADSAFE            1  /* serialized: the share serve loop reads on its own thread */
#define SQLITE_OMIT_LOAD_EXTENSION   1  /* the app never loads extensions — smaller and safer */

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
#  pragma warning(push, 0)
#endif

#include "../vendor/sqlite/sqlite3.c"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
