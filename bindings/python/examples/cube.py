"""Renders a textured cube offscreen and saves a screenshot.
    python examples/cube.py out.png [texture.png]   (HRL_LIBRARY = path of the library if needed)
"""
import sys, hrl
errs = []
def on_err(code, sev, detail):
    print("ERR", hrl.HRL_EError(code).name, detail); errs.append(code)
hrl.HRL_RegisterErrorCallback(on_err)
hrl.HRL_Init(hrl.HRL_VULKAN); hrl.HRL_InitContext(320, 240, None)
s = hrl.HRL_CreateScene(0)
cam = hrl.HRL_CreateCamera(s, hrl.HRL_PERSPECTIVE)
hrl.HRL_SetCameraLocation(cam, 0, 1.5, 5); hrl.HRL_SetCameraRotation(cam, -15, -90, 0)
hrl.HRL_SetCameraPerspectiveFov(cam, 60); hrl.HRL_SetCameraNearPlane(cam, 0.1); hrl.HRL_SetCameraFarPlane(cam, 100)
hrl.HRL_CreateViewport(s, cam, 0, 0, 1, 1)
hrl.HRL_SetSkySphereEnabled(s, 1)
V = hrl.HRL_Vertex3D
def v(p, n, uv):
    x = V(); x.position[:] = p; x.normal[:] = n; x.uv[:] = uv; return x
verts, idx = [], []
def face(n, u, w):
    b = len(verts)
    for cu, cv in ((-1,-1),(1,-1),(1,1),(-1,1)):
        x = V()
        x.position[:] = [0.5*(n[k] + cu*u[k] + cv*w[k]) for k in range(3)]
        x.normal[:] = n; x.tangent[:] = u; x.bitangent[:] = w; x.uv[:] = (cu*0.5+0.5, cv*0.5+0.5)
        verts.append(x)
    idx.extend([b, b+1, b+2, b+2, b+3, b])
for n,u,w in [((1,0,0),(0,0,-1),(0,1,0)),((-1,0,0),(0,0,1),(0,1,0)),((0,1,0),(1,0,0),(0,0,-1)),((0,-1,0),(1,0,0),(0,0,1)),((0,0,1),(1,0,0),(0,1,0)),((0,0,-1),(-1,0,0),(0,1,0))]:
    face(n,u,w)
m = hrl.HRL_CreateMesh3D(s, verts, idx); hrl.HRL_SetMeshRotation(m, 0, 30, 0)
mat = hrl.HRL_CreateMaterial(hrl.HRL_MESH_3D_SHADER); hrl.HRL_MaterialSetVec3(mat, "TintColor", 0.9, 0.3, 0.2)
(len(sys.argv) > 2) and hrl.HRL_MaterialSetTexture(mat, hrl.HRL_T_ALBEDO, hrl.HRL_CreateTexture(open(sys.argv[2], "rb").read() if len(sys.argv) > 2 else b"")); hrl.HRL_SetMeshMaterial(m, mat)
l = hrl.HRL_CreateLight(s, hrl.HRL_DIRECTIONAL_LIGHT); hrl.HRL_SetLightRotation(l, -50, 30, 0); hrl.HRL_SetLightIntensity(l, 2); hrl.HRL_SetLightColor(l, 1, 1, 1); sk = hrl.HRL_CreateLight(s, hrl.HRL_SKY_LIGHT); hrl.HRL_SetLightColor(sk, .3,.35,.5); hrl.HRL_SetLightIntensity(sk, .4)
hrl.HRL_AddScreenMessage(s, 5.0, "Hello from Python 100%")
print("loc", hrl.HRL_GetCameraLocation(cam) if hasattr(hrl, "HRL_GetCameraLocation") else "-")
print("last", hrl.HRL_GetLastError())
hrl.HRL_WaitForAllAsyncResources()
for f in range(3):
    hrl.HRL_BeginFrame()
    if f == 2: hrl.HRL_TakeScreenshot(s, (sys.argv[1] if len(sys.argv) > 1 else "cube_python.png"))
    hrl.HRL_EndFrame()
hrl.HRL_Shutdown(); print("DONE", len(errs))
