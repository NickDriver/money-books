#ifndef MB_SEED_H
#define MB_SEED_H
/*
 * Money Books — first-run seeding (D7). Two wizard paths:
 *   - "start empty"       -> mb_seed_system_accounts (the 4 engine-required SYSTEM accounts)
 *   - "starter template"  -> mb_seed_starter_chart   (full US freelancer chart, STARTER_CHART.md)
 */
#include "../store/store.h"

mb_err mb_seed_system_accounts(mb_store *s) MB_MUST_CHECK;
mb_err mb_seed_starter_chart(mb_store *s) MB_MUST_CHECK;

#endif /* MB_SEED_H */
