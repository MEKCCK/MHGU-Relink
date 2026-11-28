// Auto-generated item names: iid -> UTF-8 display name. NULL = unknown / description / dev slot.
// Even iid = name, odd iid = description (itemData_chS.gmd).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
const char *hud_name_lookup(int iid);
// Even iids that have a name, ascending. Returns the total even if out is NULL or max is smaller.
int hud_name_ids(int *out, int max);
#ifdef __cplusplus
}
#endif
