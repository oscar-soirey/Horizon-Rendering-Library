#include "hrl.h"
static void cb(HRL_id, HRL_EGizmoPart, void*) {}
int main() {
    HRL_id g = HRL_CreateGizmo(HRL_INVALID_ID);
    HRL_SetGizmoMode(g, HRL_GIZMO_MODE_TRANSLATE);
    HRL_SetGizmoSpace(g, HRL_GIZMO_SPACE_WORLD);
    HRL_SetGizmoTranslateAxes(g, HRL_GIZMO_AXIS_X|HRL_GIZMO_AXIS_Y);
    HRL_SetGizmoChangedCallback(g, cb, nullptr);
    return 0;
}
