// HRL example: a small scene editor. The interface is made with Raven
// (https://github.com/oscar-soirey/Raven), the 3D view is rendered by the
// native Vulkan backend of HRL.
//
// How the two libraries share the window: the Raven <Viewport> widget owns a
// native child window (mode 3 of raven_viewport.h). HRL creates its Vulkan
// surface and swapchain on that child window and presents there with its own
// device; Raven keeps the child window on the widget and forwards the mouse and
// keyboard events of the widget. Nothing in HRL knows about Raven.
//
// Controls (3D view):
//   left click            select an object (empty space: deselect)
//   left drag             move the selected object on the horizontal plane
//   Shift + left drag     move it up / down
//   right button + mouse  look around;  W A S D / Q E move (Shift: faster)
//   mouse wheel           move forward / backward
//   F                     look at the selection      Delete: remove it
//   Ctrl+D duplicate      Ctrl+S / Ctrl+O save / load hrl_editor_scene.txt
//
// Automated run (used for the tests of the example):
//   HRL_EDITOR_FRAMES=60 HRL_EDITOR_CAPTURE=out ./hrl_editor
//   renders 60 frames, writes out_scene.png (HRL) and out_ui.ppm (Raven),
//   checks the picking and exits.
#include <hrl.h>
#include <hrl_vulkan.h>
#include <Raven.h>

#include "native_surface.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const float kPi = 3.14159265358979f;
const float kFovY = 55.f;
const char* const kSceneFile = "hrl_editor_scene.txt";

// ---------------------------------------------------------------------------
//  Interface
// ---------------------------------------------------------------------------
const char kLayout[] = R"xml(
<Window Title="HRL Editor (Vulkan) - interface: Raven" Width="1500" Height="860" MinWidth="1000" MinHeight="600">
  <Shortcut Keys="Ctrl+D" Action="object.duplicate"/>
  <Shortcut Keys="Ctrl+S" Action="scene.save"/>
  <Shortcut Keys="Ctrl+O" Action="scene.load"/>
  <VBox>
    <HBox Spacing="6" Slot.Padding="6">
      <Button Text="+ Cube" OnClicked="add.cube"/>
      <Button Text="+ Sphere" OnClicked="add.sphere"/>
      <Button Text="+ Cylinder" OnClicked="add.cylinder"/>
      <Button Text="+ Cone" OnClicked="add.cone"/>
      <Button Text="+ Plane" OnClicked="add.plane"/>
      <Separator Orientation="Vertical"/>
      <Button Text="Duplicate" Style="Flat" OnClicked="object.duplicate" Tooltip="Ctrl+D"/>
      <Button Text="Delete" Style="Flat" OnClicked="object.delete" Tooltip="Delete"/>
      <Button Text="Focus" Style="Flat" OnClicked="object.focus" Tooltip="F"/>
      <Spacer Slot.Fill="1"/>
      <Button Text="New" Style="Flat" OnClicked="scene.new"/>
      <Button Text="Load" Style="Flat" OnClicked="scene.load" Tooltip="Ctrl+O"/>
      <Button Text="Save" Style="Primary" OnClicked="scene.save" Tooltip="Ctrl+S"/>
    </HBox>
    <Separator/>
    <HBox Slot.Fill="1">
      <VBox Width="210" Spacing="6" Slot.Padding="8">
        <Label Text="Scene" TextSize="15"/>
        <ListView Name="objects" Slot.Fill="1"/>
      </VBox>
      <Separator Orientation="Vertical"/>
      <Viewport Name="scene" Slot.Fill="1" Realtime="true"/>
      <Separator Orientation="Vertical"/>
      <ScrollBox Width="330">
        <VBox Spacing="8" Slot.Padding="8">
          <Expander Title="Object" Expanded="true">
            <Grid Name="object_panel" Columns="auto,1,1,1" Spacing="6" Slot.Padding="6">
              <Label Text="Name" Slot.Column="0" Slot.Row="0" Slot.VAlign="Center"/>
              <TextInput Name="name" Slot.Column="1" Slot.Row="0" Slot.ColumnSpan="3" OnCommitted="object.rename"/>
              <Label Text="Position" Slot.Column="0" Slot.Row="1" Slot.VAlign="Center"/>
              <SpinBox Name="px" Slot.Column="1" Slot.Row="1" Min="-1000" Max="1000" Step="0.1" Decimals="2" OnChanged="object.edit"/>
              <SpinBox Name="py" Slot.Column="2" Slot.Row="1" Min="-1000" Max="1000" Step="0.1" Decimals="2" OnChanged="object.edit"/>
              <SpinBox Name="pz" Slot.Column="3" Slot.Row="1" Min="-1000" Max="1000" Step="0.1" Decimals="2" OnChanged="object.edit"/>
              <Label Text="Rotation" Slot.Column="0" Slot.Row="2" Slot.VAlign="Center"/>
              <SpinBox Name="rx" Slot.Column="1" Slot.Row="2" Min="-360" Max="360" Step="5" Decimals="1" OnChanged="object.edit"/>
              <SpinBox Name="ry" Slot.Column="2" Slot.Row="2" Min="-360" Max="360" Step="5" Decimals="1" OnChanged="object.edit"/>
              <SpinBox Name="rz" Slot.Column="3" Slot.Row="2" Min="-360" Max="360" Step="5" Decimals="1" OnChanged="object.edit"/>
              <Label Text="Scale" Slot.Column="0" Slot.Row="3" Slot.VAlign="Center"/>
              <SpinBox Name="sx" Slot.Column="1" Slot.Row="3" Min="0.01" Max="500" Step="0.1" Decimals="2" OnChanged="object.edit"/>
              <SpinBox Name="sy" Slot.Column="2" Slot.Row="3" Min="0.01" Max="500" Step="0.1" Decimals="2" OnChanged="object.edit"/>
              <SpinBox Name="sz" Slot.Column="3" Slot.Row="3" Min="0.01" Max="500" Step="0.1" Decimals="2" OnChanged="object.edit"/>
            </Grid>
          </Expander>
          <Expander Title="Material" Expanded="true">
            <VBox Name="material_panel" Spacing="6" Slot.Padding="6">
              <ColorPicker Name="color" Alpha="false" Compact="true" Height="170" OnChanged="object.edit"/>
              <Grid Columns="auto,1" Spacing="6">
                <Label Text="Roughness" Slot.Column="0" Slot.Row="0" Slot.VAlign="Center"/>
                <Slider Name="roughness" Slot.Column="1" Slot.Row="0" Min="0.03" Max="1" Value="0.5" OnChanged="object.edit"/>
                <Label Text="Metallic" Slot.Column="0" Slot.Row="1" Slot.VAlign="Center"/>
                <Slider Name="metallic" Slot.Column="1" Slot.Row="1" Min="0" Max="1" Value="0" OnChanged="object.edit"/>
                <Label Text="Emissive" Slot.Column="0" Slot.Row="2" Slot.VAlign="Center"/>
                <Slider Name="emissive" Slot.Column="1" Slot.Row="2" Min="0" Max="10" Value="0" OnChanged="object.edit"/>
                <Label Text="Opacity" Slot.Column="0" Slot.Row="3" Slot.VAlign="Center"/>
                <Slider Name="opacity" Slot.Column="1" Slot.Row="3" Min="0.05" Max="1" Value="1" OnChanged="object.edit"/>
              </Grid>
              <Checkbox Name="translucent" Text="Translucent (uses the opacity)" Style="Switch" OnChanged="object.edit"/>
            </VBox>
          </Expander>
          <Expander Title="Lighting" Expanded="true">
            <VBox Spacing="6" Slot.Padding="6">
              <Grid Columns="auto,1" Spacing="6">
                <Label Text="Sun height" Slot.Column="0" Slot.Row="0" Slot.VAlign="Center"/>
                <Slider Name="sun_pitch" Slot.Column="1" Slot.Row="0" Min="3" Max="90" Value="48" OnChanged="env.edit"/>
                <Label Text="Sun heading" Slot.Column="0" Slot.Row="1" Slot.VAlign="Center"/>
                <Slider Name="sun_yaw" Slot.Column="1" Slot.Row="1" Min="0" Max="360" Value="35" OnChanged="env.edit"/>
                <Label Text="Sun intensity" Slot.Column="0" Slot.Row="2" Slot.VAlign="Center"/>
                <Slider Name="sun_intensity" Slot.Column="1" Slot.Row="2" Min="0" Max="15" Value="4" OnChanged="env.edit"/>
                <Label Text="Exposure" Slot.Column="0" Slot.Row="3" Slot.VAlign="Center"/>
                <Slider Name="exposure" Slot.Column="1" Slot.Row="3" Min="-4" Max="4" Value="0" OnChanged="env.edit"/>
                <Label Text="Bloom" Slot.Column="0" Slot.Row="4" Slot.VAlign="Center"/>
                <Slider Name="bloom" Slot.Column="1" Slot.Row="4" Min="0" Max="3" Value="1" OnChanged="env.edit"/>
              </Grid>
              <Checkbox Name="rt" Text="Ray traced shadows" Style="Switch" OnChanged="env.edit"/>
            </VBox>
          </Expander>
          <Label Wrap="true" Opacity="0.6" Text="Left click: select - drag: move (Shift: up / down). Right button + W A S D Q E: fly. F: focus, Delete: remove."/>
        </VBox>
      </ScrollBox>
    </HBox>
    <Separator/>
    <Label Name="status" Slot.Padding="6" Text="Starting..."/>
  </VBox>
</Window>
)xml";

// ---------------------------------------------------------------------------
//  Small vector helpers
// ---------------------------------------------------------------------------
struct Vec3 { float x = 0, y = 0, z = 0; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------------
struct MeshData
{
	std::vector<HRL_Vertex3D> vertices;
	std::vector<HRL_uint> indices;
};

HRL_Vertex3D Vertex(Vec3 p, Vec3 n, float u, float v, Vec3 t)
{
	HRL_Vertex3D r{};
	r.position[0] = p.x; r.position[1] = p.y; r.position[2] = p.z;
	r.normal[0] = n.x; r.normal[1] = n.y; r.normal[2] = n.z;
	r.uv[0] = u; r.uv[1] = v;
	r.tangent[0] = t.x; r.tangent[1] = t.y; r.tangent[2] = t.z;
	const Vec3 b = Cross(n, t);
	r.bitangent[0] = b.x; r.bitangent[1] = b.y; r.bitangent[2] = b.z;
	return r;
}

// All the shapes fit in a box of 1 x 1 x 1 centred on the origin; faces are
// counter-clockwise seen from outside.
MeshData MakeBox()
{
	MeshData m;
	const Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const Vec3 t[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
	for (int f = 0; f < 6; ++f)
	{
		const Vec3 b = Cross(n[f], t[f]);
		const HRL_uint base = (HRL_uint)m.vertices.size();
		for (int c = 0; c < 4; ++c)
		{
			const float su = (c == 1 || c == 2) ? 1.f : -1.f, sv = (c >= 2) ? 1.f : -1.f;
			m.vertices.push_back(Vertex((n[f] + t[f] * su + b * sv) * 0.5f, n[f], su * 0.5f + 0.5f, sv * 0.5f + 0.5f, t[f]));
		}
		m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
	}
	return m;
}

MeshData MakeSphere(int rings, int sectors)
{
	MeshData m;
	for (int r = 0; r <= rings; ++r)
	{
		const float v = (float)r / (float)rings, theta = v * kPi;
		for (int s = 0; s <= sectors; ++s)
		{
			const float u = (float)s / (float)sectors, phi = u * 2.f * kPi;
			const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
			m.vertices.push_back(Vertex(n * 0.5f, n, u, 1.f - v, {-std::sin(phi), 0.f, std::cos(phi)}));
		}
	}
	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			const HRL_uint a = (HRL_uint)(r * (sectors + 1) + s), b = a + (HRL_uint)sectors + 1;
			m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
		}
	return m;
}

// Cylinder (topRadius = 0.5) or cone (topRadius = 0), axis Y.
MeshData MakeCylinder(float topRadius, int sectors)
{
	MeshData m;
	const float bottomRadius = 0.5f;
	// Side: the normal leans up by the slope of the wall.
	const float slope = bottomRadius - topRadius; // height is 1
	const float inv = 1.f / std::sqrt(1.f + slope * slope);
	for (int row = 0; row < 2; ++row)
		for (int s = 0; s <= sectors; ++s)
		{
			const float u = (float)s / (float)sectors, phi = u * 2.f * kPi;
			const float radius = row == 0 ? topRadius : bottomRadius;
			const Vec3 n{std::cos(phi) * inv, slope * inv, std::sin(phi) * inv};
			m.vertices.push_back(Vertex({std::cos(phi) * radius, row == 0 ? 0.5f : -0.5f, std::sin(phi) * radius}, n,
				u, row == 0 ? 1.f : 0.f, {-std::sin(phi), 0.f, std::cos(phi)}));
		}
	for (int s = 0; s < sectors; ++s)
	{
		const HRL_uint a = (HRL_uint)s, b = a + (HRL_uint)sectors + 1;
		m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
	}
	// Caps.
	for (int cap = 0; cap < 2; ++cap)
	{
		const float radius = cap == 0 ? topRadius : bottomRadius, y = cap == 0 ? 0.5f : -0.5f;
		if (radius <= 0.f) continue;
		const Vec3 n{0.f, cap == 0 ? 1.f : -1.f, 0.f};
		const HRL_uint centre = (HRL_uint)m.vertices.size();
		m.vertices.push_back(Vertex({0.f, y, 0.f}, n, 0.5f, 0.5f, {1.f, 0.f, 0.f}));
		for (int s = 0; s <= sectors; ++s)
		{
			const float phi = (float)s / (float)sectors * 2.f * kPi;
			m.vertices.push_back(Vertex({std::cos(phi) * radius, y, std::sin(phi) * radius}, n,
				std::cos(phi) * 0.5f + 0.5f, std::sin(phi) * 0.5f + 0.5f, {1.f, 0.f, 0.f}));
		}
		for (int s = 0; s < sectors; ++s)
		{
			const HRL_uint a = centre + 1 + (HRL_uint)s;
			if (cap == 0) m.indices.insert(m.indices.end(), {centre, a + 1, a});
			else m.indices.insert(m.indices.end(), {centre, a, a + 1});
		}
	}
	return m;
}

// Square of 1 x 1 in the XZ plane, facing up (its material is two sided).
MeshData MakePlane()
{
	MeshData m;
	const Vec3 n{0, 1, 0}, t{1, 0, 0};
	m.vertices.push_back(Vertex({-0.5f, 0.f, 0.5f}, n, 0.f, 0.f, t));
	m.vertices.push_back(Vertex({0.5f, 0.f, 0.5f}, n, 1.f, 0.f, t));
	m.vertices.push_back(Vertex({0.5f, 0.f, -0.5f}, n, 1.f, 1.f, t));
	m.vertices.push_back(Vertex({-0.5f, 0.f, -0.5f}, n, 0.f, 1.f, t));
	m.indices = {0, 1, 2, 0, 2, 3};
	return m;
}

enum Shape { kCube, kSphere, kCylinder, kCone, kPlane, kShapeCount };
const char* const kShapeNames[kShapeCount] = {"Cube", "Sphere", "Cylinder", "Cone", "Plane"};

// ---------------------------------------------------------------------------
//  Editor state
// ---------------------------------------------------------------------------
struct Object
{
	std::string name;
	int shape = kCube;
	HRL_id mesh = HRL_INVALID_ID, material = HRL_INVALID_ID;
	Vec3 position, rotation, scale{1, 1, 1};
	Vec3 color{0.8f, 0.8f, 0.8f};
	float roughness = 0.5f, metallic = 0.f, emissive = 0.f, opacity = 1.f;
	bool translucent = false;
};

struct Widgets
{
	Rid window = 0, viewport = 0, list = 0, status = 0, objectPanel = 0, materialPanel = 0;
	Rid name = 0, position[3] = {}, rotation[3] = {}, scale[3] = {};
	Rid color = 0, roughness = 0, metallic = 0, emissive = 0, opacity = 0, translucent = 0;
	Rid sunPitch = 0, sunYaw = 0, sunIntensity = 0, exposure = 0, bloom = 0, rayTracing = 0;
};

struct Editor
{
	Widgets ui;
	EditorNativeWindow native;
	bool hrlReady = false, hrlFailed = false;
	int errors = 0;
	std::string lastError;

	HRL_id scene = HRL_INVALID_ID, camera = HRL_INVALID_ID, sun = HRL_INVALID_ID, post = HRL_INVALID_ID, floor = HRL_INVALID_ID;
	MeshData shapes[kShapeCount];
	std::vector<Object> objects;
	int selected = -1;
	int nameCounter[kShapeCount] = {};
	bool syncing = false; // the inspector is being filled: its "changed" actions are ignored

	// Camera.
	Vec3 camPos{-7.f, 4.5f, 8.f};
	float pitch = -22.f, yaw = -48.f;
	int viewWidth = 0, viewHeight = 0;

	// Mouse interaction in the viewport.
	bool looking = false, dragging = false, dragVertical = false;
	Vec3 dragOffset;
	float dragPlaneY = 0.f, dragLastY = 0.f;

	// Status bar / automated run.
	double statusTimer = 0.0;
	int frameCount = 0, framesSinceStatus = 0;
	int automatedFrames = 0;
	std::string capturePrefix;
	int exitCode = 0;
};

Editor g;

void OnHrlError(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	std::fprintf(stderr, "[HRL %s] %s: %s\n", HRL_SeverityEnumToString(severity), HRL_ErrorEnumToString(code), detail ? detail : "");
	if (severity == HRL_SEVERITY_ERROR || severity == HRL_SEVERITY_FATAL)
	{
		++g.errors;
		g.lastError = std::string(HRL_ErrorEnumToString(code)) + ": " + (detail ? detail : "");
	}
}

void OnRavenError(RError code, RSeverity severity, const char* detail)
{
	std::fprintf(stderr, "[Raven %s] %s: %s\n", Rseverity_to_string(severity), Rerror_to_string(code), detail ? detail : "");
}

// ---------------------------------------------------------------------------
//  Camera
// ---------------------------------------------------------------------------
struct CameraBasis { Vec3 forward, right, up; float tanHalf, aspect; };

CameraBasis GetCameraBasis()
{
	const float p = g.pitch * kPi / 180.f, y = g.yaw * kPi / 180.f;
	CameraBasis b;
	b.forward = {std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y)};
	b.right = {-std::sin(y), 0.f, std::cos(y)};
	b.up = Cross(b.right, b.forward);
	b.tanHalf = std::tan(kFovY * 0.5f * kPi / 180.f);
	b.aspect = g.viewHeight > 0 ? (float)g.viewWidth / (float)g.viewHeight : 1.f;
	return b;
}

// Direction of the ray through a pixel of the viewport (origin: top-left).
Vec3 PixelRay(float px, float py)
{
	const CameraBasis b = GetCameraBasis();
	const float nx = 2.f * px / (float)g.viewWidth - 1.f, ny = 1.f - 2.f * py / (float)g.viewHeight;
	const Vec3 d = b.forward + b.right * (nx * b.tanHalf * b.aspect) + b.up * (ny * b.tanHalf);
	return d * (1.f / Length(d));
}

// Pixel of a world position; false when it is behind the camera.
bool WorldToPixel(Vec3 world, float* px, float* py)
{
	const CameraBasis b = GetCameraBasis();
	const Vec3 d = world - g.camPos;
	const float depth = Dot(d, b.forward);
	if (depth <= 0.01f) return false;
	*px = (Dot(d, b.right) / (depth * b.tanHalf * b.aspect) * 0.5f + 0.5f) * (float)g.viewWidth;
	*py = (0.5f - Dot(d, b.up) / (depth * b.tanHalf) * 0.5f) * (float)g.viewHeight;
	return true;
}

// Intersection of a ray from the camera with the horizontal plane y = height.
bool RayToPlane(Vec3 dir, float height, Vec3* hit)
{
	if (std::fabs(dir.y) < 1e-4f) return false;
	const float t = (height - g.camPos.y) / dir.y;
	if (t <= 0.f || t > 500.f) return false;
	*hit = g.camPos + dir * t;
	return true;
}

// ---------------------------------------------------------------------------
//  Objects
// ---------------------------------------------------------------------------
void ApplyTransform(const Object& o)
{
	HRL_SetMeshLocation(o.mesh, o.position.x, o.position.y, o.position.z);
	HRL_SetMeshRotation(o.mesh, o.rotation.x, o.rotation.y, o.rotation.z);
	HRL_SetMeshScale(o.mesh, o.scale.x, o.scale.y, o.scale.z);
}

void ApplyMaterial(const Object& o)
{
	HRL_MaterialSetVec3(o.material, "TintColor", o.color.x, o.color.y, o.color.z);
	HRL_MaterialSetInt(o.material, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(o.material, "RoughnessValue", o.roughness);
	HRL_MaterialSetInt(o.material, "MetallicUseValue", 1);
	HRL_MaterialSetFloat(o.material, "MetallicValue", o.metallic);
	HRL_MaterialSetEmissiveColor(o.material, o.color.x, o.color.y, o.color.z, o.emissive);
	HRL_MaterialSetBool(o.material, HRL_MATERIAL_PARAM_TRANSLUCENT, o.translucent ? HRL_TRUE : HRL_FALSE);
	HRL_MaterialSetBool(o.material, HRL_MATERIAL_PARAM_TWO_SIDED, (o.translucent || o.shape == kPlane) ? HRL_TRUE : HRL_FALSE);
	HRL_MaterialSetInt(o.material, "OpacityUseValue", 1);
	HRL_MaterialSetFloat(o.material, "OpacityValue", o.translucent ? o.opacity : 1.f);
}

// Creates the HRL mesh and material of an object description.
void Instantiate(Object& o)
{
	const MeshData& data = g.shapes[o.shape];
	o.material = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	o.mesh = HRL_CreateMesh3D(g.scene, data.vertices.data(), data.vertices.size(), data.indices.data(), data.indices.size());
	HRL_SetMeshMaterial(o.mesh, o.material);
	ApplyMaterial(o);
	ApplyTransform(o);
}

void SyncInspector();

void Select(int index)
{
	g.selected = (index >= 0 && index < (int)g.objects.size()) ? index : -1;
	g.syncing = true;
	Rlist_view_set_selected(g.ui.list, g.selected);
	g.syncing = false;
	SyncInspector();
}

int AddObject(Object o)
{
	Instantiate(o);
	g.objects.push_back(o);
	g.syncing = true;
	Rlist_view_add_item(g.ui.list, o.name.c_str());
	g.syncing = false;
	return (int)g.objects.size() - 1;
}

void AddShape(int shape)
{
	if (!g.hrlReady) return;
	Object o;
	o.shape = shape;
	o.name = std::string(kShapeNames[shape]) + " " + std::to_string(++g.nameCounter[shape]);
	// In front of the camera, on the ground.
	const CameraBasis b = GetCameraBasis();
	Vec3 hit;
	if (!RayToPlane(b.forward, 0.f, &hit) || Length(hit - g.camPos) > 25.f) hit = g.camPos + b.forward * 8.f;
	o.position = {hit.x, shape == kPlane ? 0.01f : 0.5f, hit.z};
	const Vec3 palette[5] = {{0.85f, 0.25f, 0.2f}, {0.2f, 0.5f, 0.9f}, {0.95f, 0.75f, 0.25f}, {0.3f, 0.75f, 0.4f}, {0.8f, 0.8f, 0.82f}};
	o.color = palette[g.objects.size() % 5];
	if (shape == kPlane) o.scale = {2.f, 1.f, 2.f};
	Select(AddObject(o));
}

void DeleteSelected()
{
	if (g.selected < 0) return;
	const Object o = g.objects[(size_t)g.selected];
	HRL_DeleteMesh(o.mesh);
	HRL_DeleteMaterial(o.material);
	g.objects.erase(g.objects.begin() + g.selected);
	g.syncing = true;
	Rlist_view_remove_item(g.ui.list, g.selected);
	g.syncing = false;
	g.dragging = false;
	Select(-1);
}

void DuplicateSelected()
{
	if (g.selected < 0) return;
	Object o = g.objects[(size_t)g.selected];
	o.name += " copy";
	o.position.x += 1.f;
	o.position.z += 1.f;
	Select(AddObject(o));
}

void ClearScene()
{
	for (const Object& o : g.objects)
	{
		HRL_DeleteMesh(o.mesh);
		HRL_DeleteMaterial(o.material);
	}
	g.objects.clear();
	g.syncing = true;
	Rlist_view_clear_items(g.ui.list);
	g.syncing = false;
	g.dragging = false;
	Select(-1);
}

void FocusSelected()
{
	if (g.selected < 0) return;
	const Object& o = g.objects[(size_t)g.selected];
	const float size = std::fmax(o.scale.x, std::fmax(o.scale.y, o.scale.z));
	g.camPos = o.position - GetCameraBasis().forward * (3.f + size * 2.f);
}

// ---------------------------------------------------------------------------
//  Inspector <-> object
// ---------------------------------------------------------------------------
void SyncInspector()
{
	const bool has = g.selected >= 0;
	Rwidget_set_enabled(g.ui.objectPanel, has);
	Rwidget_set_enabled(g.ui.materialPanel, has);
	if (!has) return;
	const Object& o = g.objects[(size_t)g.selected];
	g.syncing = true;
	Rtext_input_set_text(g.ui.name, o.name.c_str());
	const float* p = &o.position.x; const float* r = &o.rotation.x; const float* s = &o.scale.x;
	for (int i = 0; i < 3; ++i)
	{
		Rspin_box_set_value(g.ui.position[i], p[i]);
		Rspin_box_set_value(g.ui.rotation[i], r[i]);
		Rspin_box_set_value(g.ui.scale[i], s[i]);
	}
	Rcolor_picker_set_color(g.ui.color, o.color.x, o.color.y, o.color.z, 1.f);
	Rslider_set_value(g.ui.roughness, o.roughness);
	Rslider_set_value(g.ui.metallic, o.metallic);
	Rslider_set_value(g.ui.emissive, o.emissive);
	Rslider_set_value(g.ui.opacity, o.opacity);
	Rcheckbox_set_checked(g.ui.translucent, o.translucent);
	g.syncing = false;
}

// Only the position, while an object is dragged in the viewport.
void SyncInspectorPosition()
{
	if (g.selected < 0) return;
	const Object& o = g.objects[(size_t)g.selected];
	g.syncing = true;
	Rspin_box_set_value(g.ui.position[0], o.position.x);
	Rspin_box_set_value(g.ui.position[1], o.position.y);
	Rspin_box_set_value(g.ui.position[2], o.position.z);
	g.syncing = false;
}

void ReadInspector()
{
	if (g.selected < 0 || g.syncing) return;
	Object& o = g.objects[(size_t)g.selected];
	float* p = &o.position.x; float* r = &o.rotation.x; float* s = &o.scale.x;
	for (int i = 0; i < 3; ++i)
	{
		p[i] = (float)Rspin_box_get_value(g.ui.position[i]);
		r[i] = (float)Rspin_box_get_value(g.ui.rotation[i]);
		s[i] = (float)Rspin_box_get_value(g.ui.scale[i]);
	}
	float alpha = 1.f;
	Rcolor_picker_get_color(g.ui.color, &o.color.x, &o.color.y, &o.color.z, &alpha);
	o.roughness = Rslider_get_value(g.ui.roughness);
	o.metallic = Rslider_get_value(g.ui.metallic);
	o.emissive = Rslider_get_value(g.ui.emissive);
	o.opacity = Rslider_get_value(g.ui.opacity);
	o.translucent = Rcheckbox_is_checked(g.ui.translucent) != 0;
	ApplyTransform(o);
	ApplyMaterial(o);
}

void ReadEnvironment()
{
	if (!g.hrlReady || g.syncing) return;
	HRL_SetLightRotation(g.sun, -Rslider_get_value(g.ui.sunPitch), Rslider_get_value(g.ui.sunYaw), 0.f);
	HRL_SetLightIntensity(g.sun, Rslider_get_value(g.ui.sunIntensity));
	HRL_MaterialSetFloat(g.post, "exposure", Rslider_get_value(g.ui.exposure));
	HRL_MaterialSetFloat(g.post, "bloomStrength", Rslider_get_value(g.ui.bloom));
	if (HRL_Vulkan_IsRayTracingSupported())
		HRL_Vulkan_SetRayTracedShadows(Rcheckbox_is_checked(g.ui.rayTracing) ? HRL_TRUE : HRL_FALSE);
}

// ---------------------------------------------------------------------------
//  Scene file (one object per line)
// ---------------------------------------------------------------------------
void SaveScene()
{
	std::FILE* file = std::fopen(kSceneFile, "w");
	if (!file) { g.lastError = std::string("cannot write ") + kSceneFile; return; }
	std::fprintf(file, "hrl_editor_scene 1\n");
	for (const Object& o : g.objects)
		std::fprintf(file, "object %d %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %d %s\n", o.shape,
			o.position.x, o.position.y, o.position.z, o.rotation.x, o.rotation.y, o.rotation.z,
			o.scale.x, o.scale.y, o.scale.z, o.color.x, o.color.y, o.color.z,
			o.roughness, o.metallic, o.emissive, o.opacity, o.translucent ? 1 : 0, o.name.c_str());
	std::fclose(file);
	g.lastError = std::string("saved ") + kSceneFile; // shown in the status bar
}

void LoadScene()
{
	std::FILE* file = std::fopen(kSceneFile, "r");
	if (!file) { g.lastError = std::string("cannot read ") + kSceneFile; return; }
	ClearScene();
	char line[1024];
	while (std::fgets(line, sizeof(line), file))
	{
		Object o;
		int translucent = 0, consumed = 0;
		if (std::sscanf(line, "object %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %d %n", &o.shape,
				&o.position.x, &o.position.y, &o.position.z, &o.rotation.x, &o.rotation.y, &o.rotation.z,
				&o.scale.x, &o.scale.y, &o.scale.z, &o.color.x, &o.color.y, &o.color.z,
				&o.roughness, &o.metallic, &o.emissive, &o.opacity, &translucent, &consumed) < 18)
			continue;
		if (o.shape < 0 || o.shape >= kShapeCount) continue;
		o.translucent = translucent != 0;
		o.name = line + consumed;
		while (!o.name.empty() && (o.name.back() == '\n' || o.name.back() == '\r')) o.name.pop_back();
		if (o.name.empty()) o.name = kShapeNames[o.shape];
		AddObject(o);
	}
	std::fclose(file);
	g.lastError = std::string("loaded ") + kSceneFile;
}

void BuildDefaultScene()
{
	struct Preset { int shape; Vec3 pos, scale, color; float roughness, metallic, emissive, opacity; bool translucent; const char* name; };
	const Preset presets[] = {
		{kCube, {0.f, 0.75f, 0.f}, {1.5f, 1.5f, 1.5f}, {0.85f, 0.25f, 0.2f}, 0.45f, 0.f, 0.f, 1.f, false, "Red cube"},
		{kSphere, {2.6f, 0.8f, 0.6f}, {1.6f, 1.6f, 1.6f}, {1.0f, 0.78f, 0.34f}, 0.2f, 1.f, 0.f, 1.f, false, "Gold sphere"},
		{kCylinder, {-2.4f, 1.f, -0.6f}, {1.f, 2.f, 1.f}, {0.9f, 0.9f, 0.88f}, 0.7f, 0.f, 0.f, 1.f, false, "Pillar"},
		{kCone, {-0.6f, 0.6f, 2.6f}, {1.2f, 1.2f, 1.2f}, {0.3f, 0.75f, 0.4f}, 0.5f, 0.f, 0.f, 1.f, false, "Cone"},
		{kSphere, {1.2f, 0.4f, 2.8f}, {0.8f, 0.8f, 0.8f}, {0.2f, 1.0f, 0.5f}, 0.4f, 0.f, 4.f, 1.f, false, "Lamp"},
		{kCube, {1.4f, 0.9f, -2.4f}, {2.4f, 1.8f, 0.06f}, {0.55f, 0.85f, 1.0f}, 0.08f, 0.f, 0.f, 0.35f, true, "Glass pane"},
	};
	for (const Preset& p : presets)
	{
		Object o;
		o.shape = p.shape; o.position = p.pos; o.scale = p.scale; o.color = p.color;
		o.roughness = p.roughness; o.metallic = p.metallic; o.emissive = p.emissive;
		o.opacity = p.opacity; o.translucent = p.translucent; o.name = p.name;
		AddObject(o);
	}
}

// ---------------------------------------------------------------------------
//  HRL start-up (first frame where the viewport has a size)
// ---------------------------------------------------------------------------
bool StartHrl(int width, int height)
{
	RNativeWindow native;
	std::memset(&native, 0, sizeof(native));
	if (!Rviewport_set_native(g.ui.viewport, R_TRUE) || !Rviewport_get_native_handle(g.ui.viewport, &native))
	{
		g.lastError = "Raven could not create the native window of the viewport";
		return false;
	}
	g.native.display = native.display;
	g.native.window = native.window;
	switch (native.system)
	{
	case R_WINDOW_SYSTEM_WIN32: g.native.system = EditorNativeWindow::Win32; break;
	case R_WINDOW_SYSTEM_X11: g.native.system = EditorNativeWindow::X11; break;
	case R_WINDOW_SYSTEM_WAYLAND: g.native.system = EditorNativeWindow::Wayland; break;
	default: g.lastError = "this window system is not supported by the example"; return false;
	}

	uint32_t extensionCount = 0;
	const char* const* extensions = EditorSurfaceExtensions(g.native, &extensionCount);
	HRL_RegisterErrorCallback(OnHrlError);
	HRL_Vulkan_SetInstanceExtensions(extensions, extensionCount);
	HRL_Vulkan_SetSurfaceCallback(EditorCreateSurface, &g.native);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext((HRL_uint)width, (HRL_uint)height, nullptr);
	if (g.errors) return false;
	g.viewWidth = width;
	g.viewHeight = height;

	g.scene = HRL_CreateScene(HRL_TRUE);
	g.camera = HRL_CreateCamera(g.scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(g.camera, kFovY);
	HRL_SetCameraNearPlane(g.camera, 0.1f);
	HRL_SetCameraFarPlane(g.camera, 500.f);
	const HRL_id viewport = HRL_CreateViewport(g.scene, g.camera, 0.f, 0.f, 1.f, 1.f);
	g.post = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_CreatePostProcess(viewport, g.post, 0);

	HRL_SetSkySphereEnabled(g.scene, HRL_TRUE);
	HRL_SetSkySphereColors(g.scene, 0.05f, 0.16f, 0.55f, 0.36f, 0.50f, 0.78f, 0.12f, 0.11f, 0.10f);
	HRL_SetEnvironmentMappingEnabled(g.scene, HRL_TRUE);

	g.sun = HRL_CreateLight(g.scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightColor(g.sun, 1.0f, 0.95f, 0.86f);
	HRL_SetLightCastShadows(g.sun, HRL_TRUE);
	HRL_SetLightShadowResolution(g.sun, 2048);

	g.shapes[kCube] = MakeBox();
	g.shapes[kSphere] = MakeSphere(32, 48);
	g.shapes[kCylinder] = MakeCylinder(0.5f, 48);
	g.shapes[kCone] = MakeCylinder(0.f, 48);
	g.shapes[kPlane] = MakePlane();

	// Ground: not listed, cannot be selected.
	const HRL_id floorMaterial = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetVec3(floorMaterial, "TintColor", 0.36f, 0.37f, 0.40f);
	HRL_MaterialSetInt(floorMaterial, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(floorMaterial, "RoughnessValue", 0.6f);
	HRL_MaterialSetInt(floorMaterial, "MetallicUseValue", 1);
	HRL_MaterialSetFloat(floorMaterial, "MetallicValue", 0.f);
	const MeshData& box = g.shapes[kCube];
	g.floor = HRL_CreateMesh3D(g.scene, box.vertices.data(), box.vertices.size(), box.indices.data(), box.indices.size());
	HRL_SetMeshMaterial(g.floor, floorMaterial);
	HRL_SetMeshScale(g.floor, 80.f, 0.5f, 80.f);
	HRL_SetMeshLocation(g.floor, 0.f, -0.25f, 0.f);

	g.hrlReady = true;
	Rwidget_set_enabled(g.ui.rayTracing, HRL_Vulkan_IsRayTracingSupported());
	ReadEnvironment();
	BuildDefaultScene();
	Select(0);
	return true;
}

// ---------------------------------------------------------------------------
//  Viewport input
// ---------------------------------------------------------------------------
int FindObjectByMesh(HRL_id mesh)
{
	for (size_t i = 0; i < g.objects.size(); ++i)
		if (g.objects[i].mesh == mesh) return (int)i;
	return -1;
}

// Object under a pixel of the viewport (origin: top-left), -1 when none.
int PickObject(float px, float py)
{
	if (px < 0.f || py < 0.f || px >= (float)g.viewWidth || py >= (float)g.viewHeight) return -1;
	HRL_EMeshType type = HRL_3D_MESH;
	// HRL picking uses a bottom-left origin.
	const HRL_id mesh = HRL_GetHoveredObject(g.scene, (int)px, g.viewHeight - 1 - (int)py, &type);
	return mesh == HRL_INVALID_ID ? -1 : FindObjectByMesh(mesh);
}

int OnViewportEvent(Rid widget, const REvent* event, void*)
{
	if (!g.hrlReady) return R_FALSE;
	const float scale = Rwindow_get_scale(g.ui.window);
	const float px = event->local_x * scale, py = event->local_y * scale;
	switch (event->type)
	{
	case R_EVENT_MOUSE_DOWN:
		Rwidget_focus(widget);
		if (event->button == R_MOUSE_RIGHT) g.looking = true;
		else if (event->button == R_MOUSE_LEFT)
		{
			Select(PickObject(px, py));
			if (g.selected >= 0)
			{
				const Object& o = g.objects[(size_t)g.selected];
				g.dragging = true;
				g.dragVertical = (event->modifiers & R_MOD_SHIFT) != 0;
				g.dragPlaneY = o.position.y;
				g.dragLastY = py;
				Vec3 hit;
				g.dragOffset = RayToPlane(PixelRay(px, py), g.dragPlaneY, &hit) ? o.position - hit : Vec3{};
			}
		}
		return R_TRUE;
	case R_EVENT_MOUSE_UP:
		if (event->button == R_MOUSE_RIGHT) g.looking = false;
		if (event->button == R_MOUSE_LEFT) g.dragging = false;
		return R_TRUE;
	case R_EVENT_MOUSE_MOVE:
		if (g.looking)
		{
			g.yaw += event->delta_x * 0.2f;
			g.pitch = Clamp(g.pitch - event->delta_y * 0.2f, -89.f, 89.f);
		}
		else if (g.dragging && g.selected >= 0)
		{
			Object& o = g.objects[(size_t)g.selected];
			if (g.dragVertical)
			{
				// One pixel moves the object by what a pixel covers at its distance.
				const float perPixel = 2.f * Length(o.position - g.camPos) * GetCameraBasis().tanHalf / (float)g.viewHeight;
				o.position.y -= (py - g.dragLastY) * perPixel;
				g.dragLastY = py;
			}
			else
			{
				Vec3 hit;
				if (RayToPlane(PixelRay(px, py), g.dragPlaneY, &hit) && Length(hit - g.camPos) < 200.f)
				{
					o.position.x = hit.x + g.dragOffset.x;
					o.position.z = hit.z + g.dragOffset.z;
				}
			}
			ApplyTransform(o);
			SyncInspectorPosition();
		}
		return R_TRUE;
	case R_EVENT_MOUSE_WHEEL:
		g.camPos = g.camPos + GetCameraBasis().forward * (event->wheel_y * (event->wheel_precise ? 0.3f : 1.f));
		return R_TRUE;
	case R_EVENT_KEY_DOWN:
		if (event->key == R_KEY_DELETE) { DeleteSelected(); return R_TRUE; }
		if (event->key == R_KEY_F) { FocusSelected(); return R_TRUE; }
		return R_FALSE;
	case R_EVENT_FOCUS_LOST:
		g.looking = false;
		g.dragging = false;
		return R_FALSE;
	default:
		return R_FALSE;
	}
}

void UpdateFlyCamera(float dt)
{
	if (!g.looking) return;
	const CameraBasis b = GetCameraBasis();
	auto key = [](RKey k) { return Rwindow_is_key_down(g.ui.window, k) ? 1.f : 0.f; };
	const float speed = (key(R_KEY_LEFT_SHIFT) > 0.f ? 15.f : 5.f) * dt;
	const float forward = key(R_KEY_W) - key(R_KEY_S), right = key(R_KEY_D) - key(R_KEY_A), up = key(R_KEY_E) - key(R_KEY_Q);
	g.camPos = g.camPos + (b.forward * forward + b.right * right + Vec3{0.f, up, 0.f}) * speed;
}

// Box around the selected object, and its local axes.
void DrawSelection()
{
	if (g.selected < 0) return;
	const Object& o = g.objects[(size_t)g.selected];
	const float rx = o.rotation.x * kPi / 180.f, ry = o.rotation.y * kPi / 180.f, rz = o.rotation.z * kPi / 180.f;
	// Same order as HRL: translate * rotateX * rotateY * rotateZ * scale.
	auto toWorld = [&](Vec3 v)
	{
		v = {v.x * o.scale.x, v.y * o.scale.y, v.z * o.scale.z};
		v = {v.x * std::cos(rz) - v.y * std::sin(rz), v.x * std::sin(rz) + v.y * std::cos(rz), v.z};
		v = {v.x * std::cos(ry) + v.z * std::sin(ry), v.y, -v.x * std::sin(ry) + v.z * std::cos(ry)};
		v = {v.x, v.y * std::cos(rx) - v.z * std::sin(rx), v.y * std::sin(rx) + v.z * std::cos(rx)};
		return v + o.position;
	};
	auto line = [&](Vec3 a, Vec3 b, float r, float gr, float bl)
	{
		HRL_DrawDebugSegment(g.scene, a.x, a.y, a.z, b.x, b.y, b.z, r, gr, bl);
	};
	const float h = 0.52f, y0 = o.shape == kPlane ? -0.01f : -h, y1 = o.shape == kPlane ? 0.01f : h;
	Vec3 c[8];
	for (int i = 0; i < 8; ++i) c[i] = toWorld({(i & 1) ? h : -h, (i & 2) ? y1 : y0, (i & 4) ? h : -h});
	const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (const auto& e : edges) line(c[e[0]], c[e[1]], 1.f, 0.6f, 0.1f);
	const Vec3 origin = toWorld({});
	const float axis = 0.9f;
	line(origin, toWorld({axis / o.scale.x, 0, 0}), 1.f, 0.2f, 0.2f);
	line(origin, toWorld({0, axis / o.scale.y, 0}), 0.2f, 1.f, 0.2f);
	line(origin, toWorld({0, 0, axis / o.scale.z}), 0.3f, 0.5f, 1.f);
}

void UpdateStatus(double dt)
{
	g.statusTimer += dt;
	++g.framesSinceStatus;
	if (g.statusTimer < 0.5) return;
	char text[512];
	uint64_t used = 0, budget = 0;
	HRL_Vulkan_GetMemoryUsage(&used, &budget);
	std::snprintf(text, sizeof(text), "%s  |  %.0f fps  |  %d objects  |  %d x %d  |  GPU memory %.0f MB  |  shadows: %s%s%s",
		HRL_Vulkan_GetDeviceName(), (double)g.framesSinceStatus / g.statusTimer, (int)g.objects.size(),
		g.viewWidth, g.viewHeight, (double)used / (1024.0 * 1024.0),
		HRL_Vulkan_GetRayTracedShadows() ? "ray traced" : "shadow maps",
		g.lastError.empty() ? "" : "  |  ", g.lastError.c_str());
	Rlabel_set_text(g.ui.status, text);
	g.statusTimer = 0.0;
	g.framesSinceStatus = 0;
}

// ---------------------------------------------------------------------------
//  Frame
// ---------------------------------------------------------------------------
void OnViewportResize(Rid, int width, int height, void*)
{
	if (!g.hrlReady || width <= 0 || height <= 0) return;
	if (width == g.viewWidth && height == g.viewHeight) return;
	g.viewWidth = width;
	g.viewHeight = height;
	HRL_WindowResizeCallback(width, height);
}

void OnFrame(double dt, void*)
{
	if (g.hrlFailed || !Rwindow_is_valid(g.ui.window)) return;
	int width = 0, height = 0;
	Rviewport_get_pixel_size(g.ui.viewport, &width, &height);
	if (width <= 0 || height <= 0) return;
	if (!g.hrlReady)
	{
		if (!StartHrl(width, height))
		{
			g.hrlFailed = true;
			g.exitCode = 1;
			Rlabel_set_text(g.ui.status, ("HRL could not start: " + g.lastError).c_str());
			if (g.automatedFrames) Rquit(1);
			return;
		}
	}
	OnViewportResize(g.ui.viewport, width, height, nullptr);

	UpdateFlyCamera((float)dt);
	HRL_SetCameraLocation(g.camera, g.camPos.x, g.camPos.y, g.camPos.z);
	HRL_SetCameraRotation(g.camera, g.pitch, g.yaw, 0.f);

	HRL_BeginFrame();
	DrawSelection();
	if (g.automatedFrames && g.frameCount == g.automatedFrames - 2 && !g.capturePrefix.empty())
		HRL_TakeScreenshot(g.scene, (g.capturePrefix + "_scene.png").c_str());
	HRL_EndFrame();
	++g.frameCount;
	UpdateStatus(dt);
}

// Stops HRL while the child window of the viewport still exists.
void StopHrl()
{
	if (g.hrlReady) HRL_Shutdown();
	g.hrlReady = false;
	g.hrlFailed = true; // no more frames
}

int OnWindowClose(Rid, void*)
{
	StopHrl();
	return R_TRUE;
}

void OnAction(Rid, const char* action, void*)
{
	if (!g.hrlReady || g.syncing) return;
	const std::string a = action ? action : "";
	if (a == "add.cube") AddShape(kCube);
	else if (a == "add.sphere") AddShape(kSphere);
	else if (a == "add.cylinder") AddShape(kCylinder);
	else if (a == "add.cone") AddShape(kCone);
	else if (a == "add.plane") AddShape(kPlane);
	else if (a == "object.duplicate") DuplicateSelected();
	else if (a == "object.delete") DeleteSelected();
	else if (a == "object.focus") FocusSelected();
	else if (a == "object.edit") ReadInspector();
	else if (a == "object.rename")
	{
		if (g.selected < 0) return;
		const char* text = Rtext_input_get_text(g.ui.name);
		if (!text || !*text) return;
		g.objects[(size_t)g.selected].name = text;
		Rlist_view_set_item_text(g.ui.list, g.selected, text);
	}
	else if (a == "env.edit") ReadEnvironment();
	else if (a == "scene.save") SaveScene();
	else if (a == "scene.load") LoadScene();
	else if (a == "scene.new") ClearScene();
}

void OnListSelection(Rid, int index, void*)
{
	if (g.syncing || !g.hrlReady) return;
	g.selected = (index >= 0 && index < (int)g.objects.size()) ? index : -1;
	SyncInspector();
}

bool FindWidgets()
{
	Widgets& w = g.ui;
	bool ok = true;
	auto find = [&](const char* name)
	{
		const Rid id = Rwidget_find(w.window, name);
		if (id == R_INVALID_ID || id == 0) { std::fprintf(stderr, "editor layout: widget '%s' not found\n", name); ok = false; }
		return id;
	};
	w.viewport = find("scene"); w.list = find("objects"); w.status = find("status");
	w.objectPanel = find("object_panel"); w.materialPanel = find("material_panel");
	w.name = find("name");
	const char* const pos[3] = {"px", "py", "pz"}; const char* const rot[3] = {"rx", "ry", "rz"}; const char* const scl[3] = {"sx", "sy", "sz"};
	for (int i = 0; i < 3; ++i) { w.position[i] = find(pos[i]); w.rotation[i] = find(rot[i]); w.scale[i] = find(scl[i]); }
	w.color = find("color"); w.roughness = find("roughness"); w.metallic = find("metallic");
	w.emissive = find("emissive"); w.opacity = find("opacity"); w.translucent = find("translucent");
	w.sunPitch = find("sun_pitch"); w.sunYaw = find("sun_yaw"); w.sunIntensity = find("sun_intensity");
	w.exposure = find("exposure"); w.bloom = find("bloom"); w.rayTracing = find("rt");
	return ok;
}

// Automated run: checks that clicking on the centre of the first object selects it.
bool CheckPicking()
{
	if (g.objects.empty()) return false;
	float px = 0.f, py = 0.f;
	if (!WorldToPixel(g.objects[0].position, &px, &py)) return false;
	const int picked = PickObject(px, py);
	std::printf("picking: pixel (%.0f, %.0f) -> %s\n", px, py, picked >= 0 ? g.objects[(size_t)picked].name.c_str() : "nothing");
	return picked == 0;
}

void SaveWindowCapture(const std::string& path)
{
	int width = 0, height = 0;
	uint8_t* pixels = Rwindow_capture(g.ui.window, &width, &height);
	std::FILE* file = pixels ? std::fopen(path.c_str(), "wb") : nullptr;
	if (file)
	{
		std::fprintf(file, "P6\n%d %d\n255\n", width, height);
		for (int i = 0; i < width * height; ++i) std::fwrite(pixels + i * 4, 1, 3, file);
		std::fclose(file);
	}
	Rpixels_free(pixels);
}
} // namespace

int main()
{
	if (const char* frames = std::getenv("HRL_EDITOR_FRAMES")) g.automatedFrames = std::atoi(frames);
	if (const char* capture = std::getenv("HRL_EDITOR_CAPTURE")) g.capturePrefix = capture;

	RInitDesc desc;
	std::memset(&desc, 0, sizeof(desc));
	desc.backend = R_BACKEND_VULKAN;
	desc.app_name = "HRL Editor";
	Rerror_set_callback(OnRavenError);
	if (!Rinit_ex(&desc))
	{
		std::fprintf(stderr, "hrl_editor: cannot start Raven (is a Vulkan driver installed?)\n");
		return 1;
	}

	g.ui.window = Rwindow_create_from_xml(kLayout, sizeof(kLayout) - 1);
	if (g.ui.window == R_INVALID_ID || g.ui.window == 0 || !FindWidgets())
	{
		std::fprintf(stderr, "hrl_editor: cannot create the window\n");
		Rshutdown();
		return 1;
	}
	Rwidget_set_enabled(g.ui.objectPanel, R_FALSE);
	Rwidget_set_enabled(g.ui.materialPanel, R_FALSE);
	Raction_set_default_handler(OnAction, nullptr);
	Rlist_view_set_selection_changed_callback(g.ui.list, OnListSelection, nullptr);
	Rwidget_set_event_callback(g.ui.viewport, OnViewportEvent, nullptr);
	Rviewport_set_resize_callback(g.ui.viewport, OnViewportResize, nullptr);
	Rwindow_set_close_callback(g.ui.window, OnWindowClose, nullptr);
	Rset_frame_callback(OnFrame, nullptr);

	if (g.automatedFrames > 0)
	{
		for (int i = 0; i < g.automatedFrames + 30 && g.frameCount < g.automatedFrames && Ris_running(); ++i)
		{
			Rwindow_request_redraw(g.ui.window);
			Rwait_frame(0.01);
		}
		if (!g.hrlReady || g.errors) g.exitCode = 1;
		else
		{
			if (!CheckPicking()) { std::fprintf(stderr, "hrl_editor: picking check failed\n"); g.exitCode = 1; }
			if (!g.capturePrefix.empty() && Rwindow_is_valid(g.ui.window)) SaveWindowCapture(g.capturePrefix + "_ui.ppm");
			std::printf("hrl_editor: %d frames, %d objects, %d HRL errors\n", g.frameCount, (int)g.objects.size(), g.errors);
		}
	}
	else
	{
		while (Rwait_frame(-1.0)) {}
	}

	Rset_frame_callback(nullptr, nullptr);
	StopHrl();
	Rshutdown();
	return g.exitCode ? g.exitCode : (g.errors ? 1 : 0);
}
