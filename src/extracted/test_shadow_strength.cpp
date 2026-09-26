#include "hrl.h"
int main(){
    HRL_InitContext(1,1,nullptr);
    HRL_id s = HRL_CreateScene(HRL_TRUE);
    HRL_id l = HRL_CreateLight(s, HRL_POINT_LIGHT);
    HRL_SetLightShadowStrength(l, 0.35f);
    HRL_SetLightShadowStrength(l, 0.0f);
    HRL_SetLightShadowStrength(l, 1.0f);
    HRL_DeleteLight(l);
    HRL_DeleteScene(s);
    return 0;
}
