#include "hrl.h"
static void cb(HRL_id id, HRL_EGizmoPart part, void* u) {(void)id;(void)part;(void)u;}
int main(void) {
    HRL_id g = HRL_CreateGizmo(HRL_INVALID_ID);
    HRL_SetGizmoMode(g, HRL_GIZMO_MODE_TRANSLATE);
    HRL_SetGizmoChangedCallback(g, cb, 0);
    return 0;
}
