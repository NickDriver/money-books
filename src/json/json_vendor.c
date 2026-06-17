/*
 * Compile vendored cJSON in its own translation unit with our strict warnings
 * silenced (third-party code shouldn't have to satisfy -Werror -Wpedantic).
 * The real cJSON.c lives under src/vendor and is excluded from the normal source glob.
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../vendor/cjson/cJSON.c"
#pragma clang diagnostic pop
