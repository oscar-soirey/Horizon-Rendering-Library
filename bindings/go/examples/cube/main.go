// Renders a textured cube offscreen and saves a screenshot.
//
//	go run ./examples/cube out.png [texture.png]
package main

import (
	"fmt"
	"os"

	"github.com/oscar-soirey/hrl/bindings/go/hrl"
)

func face(v *[]hrl.Vertex3D, idx *[]uint32, n, u, w [3]float32) {
	b := uint32(len(*v))
	for _, c := range [][2]float32{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}} {
		var x hrl.Vertex3D
		for k := 0; k < 3; k++ {
			x.Position[k] = 0.5 * (n[k] + c[0]*u[k] + c[1]*w[k])
			x.Normal[k], x.Tangent[k], x.Bitangent[k] = n[k], u[k], w[k]
		}
		x.Uv = [2]float32{c[0]*0.5 + 0.5, c[1]*0.5 + 0.5}
		*v = append(*v, x)
	}
	*idx = append(*idx, b, b+1, b+2, b+2, b+3, b)
}

func main() {
	out := "cube_go.png"
	if len(os.Args) > 1 {
		out = os.Args[1]
	}
	hrl.RegisterErrorCallback(func(code hrl.EError, sev hrl.ESeverity, detail string) {
		fmt.Fprintf(os.Stderr, "[HRL %d] %d: %s\n", sev, code, detail)
	})
	hrl.Init(hrl.HRL_VULKAN)
	hrl.InitContext(320, 240, nil)
	s := hrl.CreateScene(0)
	cam := hrl.CreateCamera(s, hrl.HRL_PERSPECTIVE)
	hrl.SetCameraLocation(cam, 0, 1.5, 5)
	hrl.SetCameraRotation(cam, -15, -90, 0)
	hrl.SetCameraPerspectiveFov(cam, 60)
	hrl.CreateViewport(s, cam, 0, 0, 1, 1)
	hrl.SetSkySphereEnabled(s, 1)
	var v []hrl.Vertex3D
	var i []uint32
	faces := [][3][3]float32{{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
		{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}}
	for _, f := range faces {
		face(&v, &i, f[0], f[1], f[2])
	}
	mesh := hrl.CreateMesh3D(s, v, i)
	hrl.SetMeshRotation(mesh, 0, 30, 0)
	mat := hrl.CreateMaterial(hrl.HRL_MESH_3D_SHADER)
	hrl.MaterialSetVec3(mat, "TintColor", 0.3, 0.5, 1.0)
	if len(os.Args) > 2 {
		data, err := os.ReadFile(os.Args[2])
		if err == nil {
			hrl.MaterialSetTexture(mat, hrl.HRL_T_ALBEDO, hrl.CreateTexture(data))
		}
	}
	hrl.SetMeshMaterial(mesh, mat)
	l := hrl.CreateLight(s, hrl.HRL_DIRECTIONAL_LIGHT)
	hrl.SetLightRotation(l, -50, 30, 0)
	hrl.SetLightIntensity(l, 2)
	hrl.AddScreenMessage(s, 5, "Hello from Go 100%")
	code, detail, sev := hrl.GetLastError()
	fmt.Println("last error:", code, detail, sev)
	hrl.WaitForAllAsyncResources()
	for f := 0; f < 3; f++ {
		hrl.BeginFrame()
		if f == 2 {
			hrl.TakeScreenshot(s, out)
		}
		hrl.EndFrame()
	}
	hrl.Shutdown()
	fmt.Println("DONE")
}
