// Renders a cube offscreen and saves a screenshot.
//   dotnet run -- out.png [texture.png]   (hrldll.dll / libhrldll.so next to the executable)
using System;
using System.Collections.Generic;
using System.IO;
using HRL;

static class Program
{
    static void Face(List<HRL_Vertex3D> v, List<uint> idx, float[] n, float[] u, float[] w)
    {
        uint b = (uint)v.Count;
        float[,] c = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int i = 0; i < 4; ++i)
        {
            var x = new HRL_Vertex3D();
            unsafe
            {
                for (int k = 0; k < 3; ++k)
                {
                    x.position[k] = 0.5f * (n[k] + c[i, 0] * u[k] + c[i, 1] * w[k]);
                    x.normal[k] = n[k]; x.tangent[k] = u[k]; x.bitangent[k] = w[k];
                }
                x.uv[0] = c[i, 0] * 0.5f + 0.5f; x.uv[1] = c[i, 1] * 0.5f + 0.5f;
            }
            v.Add(x);
        }
        idx.AddRange(new[] { b, b + 1, b + 2, b + 2, b + 3, b });
    }

    static unsafe void Main(string[] args)
    {
        string output = args.Length > 0 ? args[0] : "cube_csharp.png";
        HRL_CErrorCallback onError = (code, sev, detail) =>
            Console.Error.WriteLine($"[HRL {sev}] {code}: {System.Runtime.InteropServices.Marshal.PtrToStringUTF8((IntPtr)detail)}");
        Hrl.RegisterErrorCallback(onError);
        Hrl.Init(HRL_E_APIs.HRL_VULKAN);
        Hrl.InitContext(320, 240, null);
        uint s = Hrl.CreateScene(0);
        uint cam = Hrl.CreateCamera(s, HRL_ECameraType.HRL_PERSPECTIVE);
        Hrl.SetCameraLocation(cam, 0, 1.5f, 5);
        Hrl.SetCameraRotation(cam, -15, -90, 0);
        Hrl.SetCameraPerspectiveFov(cam, 60);
        Hrl.CreateViewport(s, cam, 0, 0, 1, 1);
        Hrl.SetSkySphereEnabled(s, 1);
        var v = new List<HRL_Vertex3D>(); var i = new List<uint>();
        Face(v, i, new float[] { 1, 0, 0 }, new float[] { 0, 0, -1 }, new float[] { 0, 1, 0 });
        Face(v, i, new float[] { -1, 0, 0 }, new float[] { 0, 0, 1 }, new float[] { 0, 1, 0 });
        Face(v, i, new float[] { 0, 1, 0 }, new float[] { 1, 0, 0 }, new float[] { 0, 0, -1 });
        Face(v, i, new float[] { 0, -1, 0 }, new float[] { 1, 0, 0 }, new float[] { 0, 0, 1 });
        Face(v, i, new float[] { 0, 0, 1 }, new float[] { 1, 0, 0 }, new float[] { 0, 1, 0 });
        Face(v, i, new float[] { 0, 0, -1 }, new float[] { -1, 0, 0 }, new float[] { 0, 1, 0 });
        uint mesh = Hrl.CreateMesh3D(s, v.ToArray(), i.ToArray());
        Hrl.SetMeshRotation(mesh, 0, 30, 0);
        uint mat = Hrl.CreateMaterial(Hrl.HRL_MESH_3D_SHADER);
        Hrl.MaterialSetVec3(mat, "TintColor", 0.6f, 0.3f, 1.0f);
        if (args.Length > 1)
            Hrl.MaterialSetTexture(mat, Hrl.HRL_T_ALBEDO, Hrl.CreateTexture(File.ReadAllBytes(args[1])));
        Hrl.SetMeshMaterial(mesh, mat);
        uint l = Hrl.CreateLight(s, HRL_ELightType.HRL_DIRECTIONAL_LIGHT);
        Hrl.SetLightRotation(l, -50, 30, 0);
        Hrl.SetLightIntensity(l, 2);
        Hrl.AddScreenMessageText(s, 5, "Hello from C#");
        var err = Hrl.GetLastError(out string detail, out HRL_ESeverity sev);
        Console.WriteLine($"last error: {err} {detail} {sev}");
        Hrl.WaitForAllAsyncResources();
        for (int f = 0; f < 3; ++f)
        {
            Hrl.BeginFrame();
            if (f == 2) Hrl.TakeScreenshot(s, output);
            Hrl.EndFrame();
        }
        Hrl.Shutdown();
        GC.KeepAlive(onError);
        Console.WriteLine("DONE");
    }
}
