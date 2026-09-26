#include "hrl.h"

int main()
{
    HRL_id system = HRL_CreateVFXSystem(HRL_INVALID_ID);
    HRL_id emitter = HRL_CreateVFXEmitter(system);
    HRL_id color = HRL_CreateVFXColorCurve(emitter);
    HRL_AddVFXColorKey(color, 0.f, 1.f, 0.5f, 0.1f, 1.f);
    HRL_SetVFXEmitterRenderMode(emitter, HRL_VFX_RENDER_STRETCHED_BILLBOARD);
    HRL_SetVFXEmitterBlendMode(emitter, HRL_VFX_BLEND_ADDITIVE);
    // Mesh mode accepts a static HRL_3D_MESH as a per-particle template.
    HRL_SetVFXEmitterRenderMode(emitter, HRL_VFX_RENDER_MESH);
    HRL_SetVFXEmitterMesh(emitter, HRL_INVALID_ID); // clear template in this API-only test
    HRL_SetVFXEmitterMeshScale(emitter, 0.25f, 0.25f, 0.25f);
    HRL_SetVFXEmitterMeshRotation(emitter, 0.f, 90.f, 0.f);
    HRL_SetVFXEmitterSpawnShape(emitter, HRL_VFX_SHAPE_CONE);
    HRL_SetVFXEmitterInitialVelocity(emitter, -1.f, 1.f, -1.f, 1.f, 3.f, 1.f);
    HRL_PlayVFXSystem(system);
    HRL_UpdateVFX(1.f / 60.f);
    return 0;
}
