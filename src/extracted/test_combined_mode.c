#include "hrl.h"
void f(HRL_id g){
    HRL_SetGizmoMode(g, (HRL_EGizmoMode)(HRL_GIZMO_MODE_TRANSLATE | HRL_GIZMO_MODE_ROTATE));
    HRL_SetGizmoMode(g, (HRL_EGizmoMode)(HRL_GIZMO_MODE_TRANSLATE | HRL_GIZMO_MODE_ROTATE | HRL_GIZMO_MODE_SCALE));
    HRL_EGizmoOperation op = HRL_GetGizmoActiveOperation(g); (void)op;
}
