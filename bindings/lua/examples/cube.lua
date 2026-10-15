-- Renders a textured cube offscreen and saves a screenshot.
--   luajit examples/cube.lua out.png [texture.png]   (LUA_PATH="./?.lua", HRL_LIBRARY=path of the library)
local hrl = require("hrl")
local ffi = hrl.ffi
local out, texpath = arg[1] or "cube_lua.png", arg[2]

hrl.RegisterErrorCallback(function(code, sev, detail)
  io.stderr:write(("[HRL %d] %d: %s\n"):format(sev, code, detail ~= nil and ffi.string(detail) or ""))
end)
hrl.Init(hrl.HRL_VULKAN)
hrl.InitContext(320, 240, nil)
local s = hrl.CreateScene(0)
local cam = hrl.CreateCamera(s, hrl.HRL_PERSPECTIVE)
hrl.SetCameraLocation(cam, 0, 1.5, 5)
hrl.SetCameraRotation(cam, -15, -90, 0)
hrl.SetCameraPerspectiveFov(cam, 60)
hrl.CreateViewport(s, cam, 0, 0, 1, 1)
hrl.SetSkySphereEnabled(s, 1)

local verts, idx = {}, {}
local function face(n, u, w)
  local b = #verts
  for _, c in ipairs({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) do
    local x = ffi.new("HRL_Vertex3D")
    for k = 0, 2 do
      x.position[k] = 0.5 * (n[k + 1] + c[1] * u[k + 1] + c[2] * w[k + 1])
      x.normal[k], x.tangent[k], x.bitangent[k] = n[k + 1], u[k + 1], w[k + 1]
    end
    x.uv[0], x.uv[1] = c[1] * 0.5 + 0.5, c[2] * 0.5 + 0.5
    verts[#verts + 1] = x
  end
  for _, k in ipairs({0, 1, 2, 2, 3, 0}) do idx[#idx + 1] = b + k end
end
face({1, 0, 0}, {0, 0, -1}, {0, 1, 0}); face({-1, 0, 0}, {0, 0, 1}, {0, 1, 0})
face({0, 1, 0}, {1, 0, 0}, {0, 0, -1}); face({0, -1, 0}, {1, 0, 0}, {0, 0, 1})
face({0, 0, 1}, {1, 0, 0}, {0, 1, 0}); face({0, 0, -1}, {-1, 0, 0}, {0, 1, 0})

local mesh = hrl.CreateMesh3D(s, verts, idx)
hrl.SetMeshRotation(mesh, 0, 30, 0)
local mat = hrl.CreateMaterial(hrl.HRL_MESH_3D_SHADER)
hrl.MaterialSetVec3(mat, "TintColor", 1.0, 0.8, 0.2)
if texpath then
  local f = assert(io.open(texpath, "rb")); local data = f:read("*a"); f:close()
  hrl.MaterialSetTexture(mat, hrl.HRL_T_ALBEDO, hrl.CreateTexture(data))
end
hrl.SetMeshMaterial(mesh, mat)
local l = hrl.CreateLight(s, hrl.HRL_DIRECTIONAL_LIGHT)
hrl.SetLightRotation(l, -50, 30, 0)
hrl.SetLightIntensity(l, 2)
hrl.AddScreenMessage(s, 5, "Hello from Lua 100%")
print("last error:", hrl.GetLastError())
hrl.WaitForAllAsyncResources()
for f = 0, 2 do
  hrl.BeginFrame()
  if f == 2 then hrl.TakeScreenshot(s, out) end
  hrl.EndFrame()
end
hrl.Shutdown()
print("DONE")
