#include "hrl.h"
void f(HRL_id g){
    HRL_SetGizmoMode(g, (HRL_EGizmoMode)(HRL_GIZMO_MODE_TRANSLATE | HRL_GIZMO_MODE_ROTATE));
    HRL_SetGizmoMode(g, (HRL_EGizmoMode)(HRL_GIZMO_MODE_TRANSLATE | HRL_GIZMO_MODE_ROTATE | HRL_GIZMO_MODE_SCALE));
    auto m = HRL_GetGizmoMode(g); (void)m;
    auto op = HRL_GetGizmoHoveredOperation(g); (void)op;
    HRL_SetGizmoRotateArcDegrees(g, 90.f);
}
