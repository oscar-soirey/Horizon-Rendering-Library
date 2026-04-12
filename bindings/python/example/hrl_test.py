import sys
import glfw
import ctypes

sys.path.append('../')
import hrl

# Delta time
dt = 0.0
last_time = 0.0

def calculate_delta_time():
    global dt, last_time
    current_time = glfw.get_time()
    dt = current_time - last_time
    last_time = current_time

# Camera
camera_speed = 5.0
mouse_sensitivity = 0.1
yaw = 0.0
pitch = 0.0
cam_x = 0.0
cam_y = 0.0
cam_z = 5.0
last_mouse_x = 0.0
last_mouse_y = 0.0
first_mouse = True

def process_camera_movement(window):
    global cam_x, cam_y, cam_z
    velocity = camera_speed * dt

    if glfw.get_key(window, glfw.KEY_W) == glfw.PRESS:
        cam_z -= velocity
    if glfw.get_key(window, glfw.KEY_S) == glfw.PRESS:
        cam_z += velocity
    if glfw.get_key(window, glfw.KEY_A) == glfw.PRESS:
        cam_x -= velocity
    if glfw.get_key(window, glfw.KEY_D) == glfw.PRESS:
        cam_x += velocity

def process_camera_rotation(window):
    global yaw, pitch, last_mouse_x, last_mouse_y, first_mouse

    mouse_x, mouse_y = glfw.get_cursor_pos(window)

    if first_mouse:
        last_mouse_x = mouse_x
        last_mouse_y = mouse_y
        first_mouse = False

    offset_x = (mouse_x - last_mouse_x) * mouse_sensitivity
    offset_y = (last_mouse_y - mouse_y) * mouse_sensitivity

    last_mouse_x = mouse_x
    last_mouse_y = mouse_y

    yaw   += offset_x * 0.5
    pitch += offset_y

    if pitch >  89.0: pitch =  89.0
    if pitch < -89.0: pitch = -89.0

# Callbacks
def framebuffer_size_callback(window, width, height):
    hrl.HRL_WindowResizeCallback(width, height)

def open_file(path):
    with open(path, 'rb') as f:
        data = f.read()
    return data

# Init HRL
hrl.HRL_Init(hrl.HRL_OPENGL_33)

# Init GLFW
if not glfw.init():
    exit()

window = glfw.create_window(1280, 720, "HRL Example", None, None)
glfw.make_context_current(window)
glfw.set_framebuffer_size_callback(window, framebuffer_size_callback)
glfw.set_input_mode(window, glfw.CURSOR, glfw.CURSOR_DISABLED)
glfw.swap_interval(0)

# Loader
glfw_lib = ctypes.cdll.LoadLibrary(glfw._glfw._name)
loader = ctypes.cast(glfw_lib.glfwGetProcAddress, ctypes.c_void_p).value

hrl.HRL_InitContext(1280, 720, loader)
hrl.HRL_SetDebugLineThickness(3.0)

# Scene, camera, viewport
scene    = hrl.HRL_CreateScene(True)
camera   = hrl.HRL_CreateCamera(scene, hrl.HRL_PERSPECTIVE)
hrl.HRL_SetCameraPerspectiveFov(camera, 90.0)
viewport = hrl.HRL_CreateViewport(scene, camera, 0.0, 0.0, 1.0, 1.0)

# Light
light = hrl.HRL_CreateLight(scene, hrl.HRL_POINT_LIGHT)
hrl.HRL_SetLightAttenuation(light, 0.02)
hrl.HRL_SetLightLocation(light, 0.0, 0.0, 12.0)
hrl.HRL_SetLightIntensity(light, 2.0)
hrl.HRL_SetLightColor(light, 1.0, 1.0, 1.0)
hrl.HRL_SetLightRotation(light, 0.0, 0.0, 0.0)

# Atlas sprite
atlas_data = open_file("atlas.png")
atlas_texture  = hrl.HRL_CreateTexture(atlas_data, len(atlas_data))
atlas_material = hrl.HRL_CreateMaterial(hrl.HRL_SPRITE_SHADER)
hrl.HRL_MaterialSetTexture(atlas_material, hrl.HRL_T_ALBEDO, atlas_texture)
atlas_mesh = hrl.HRL_CreateMeshSprite(scene)
hrl.HRL_SetMeshMaterial(atlas_mesh, atlas_material)

# Text
font_data      = open_file("JetBrainsMono.ttf")
jetbrains_font = hrl.HRL_CreateFont(font_data, len(font_data))
text_texture   = hrl.HRL_CreateTextureFromText("Hello world!", jetbrains_font, 55, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0)
text_material  = hrl.HRL_CreateMaterial(hrl.HRL_SPRITE_SHADER)
hrl.HRL_MaterialSetTexture(text_material, hrl.HRL_T_ALBEDO, text_texture)
text_sprite    = hrl.HRL_CreateMeshSprite(scene)
hrl.HRL_SetMeshMaterial(text_sprite, text_material)
w, h = hrl.HRL_GetTextureSize(text_texture)
hrl.HRL_SetMeshScale(text_sprite, w / 20, h / 20, 1.0)

# Post process
pp_material = hrl.HRL_CreateMaterial(hrl.HRL_DEFAULT_POST_PROCESS_SHADER)
hrl.HRL_MaterialSetFloat(pp_material, "saturation", 1.5)
pp = hrl.HRL_CreatePostProcess(viewport, pp_material, 1)

# Rock texture
al_data          = open_file("rock/albedo.jpg")
rock_texture     = hrl.HRL_CreateTexture(al_data, len(al_data))
hrl.HRL_SetTextureMinFilter(rock_texture, hrl.HRL_FILTER_TRILINEAR)
hrl.HRL_SetTextureMagFilter(rock_texture, hrl.HRL_FILTER_TRILINEAR)

normal_data      = open_file("rock/normal.jpg")
normal_texture   = hrl.HRL_CreateTexture(normal_data, len(normal_data))
hrl.HRL_SetTextureMinFilter(normal_texture, hrl.HRL_FILTER_TRILINEAR)
hrl.HRL_SetTextureMagFilter(normal_texture, hrl.HRL_FILTER_TRILINEAR)

roughness_data    = open_file("rock/roughness.jpg")
roughness_texture = hrl.HRL_CreateTexture(roughness_data, len(roughness_data))
hrl.HRL_SetTextureMinFilter(roughness_texture, hrl.HRL_FILTER_TRILINEAR)
hrl.HRL_SetTextureMagFilter(roughness_texture, hrl.HRL_FILTER_TRILINEAR)

rock_material = hrl.HRL_CreateMaterial(hrl.HRL_SPRITE_SHADER)
hrl.HRL_MaterialSetTexture(rock_material, hrl.HRL_T_ALBEDO,     rock_texture)
hrl.HRL_MaterialSetTexture(rock_material, hrl.HRL_T_NORMAL,     normal_texture)
hrl.HRL_MaterialSetTexture(rock_material, hrl.HRL_T_ROUGHNESS,  roughness_texture)
hrl.HRL_MaterialSetTexture(rock_material, hrl.HRL_T_SPECULAR,   roughness_texture)

rock_mesh = hrl.HRL_CreateMeshSprite(scene)
hrl.HRL_SetMeshMaterial(rock_mesh, rock_material)
hrl.HRL_SetMeshScale(rock_mesh, 20.0, 10.0, 1.0)
hrl.HRL_SetMeshLocation(rock_mesh, 0.0, 0.0, 1.0)

# Fog
hrl.HRL_SetFogEnabled(scene, False)
hrl.HRL_SetFogMode(scene, hrl.HRL_FOG_EXPONENTIAL)
hrl.HRL_SetFogColor(scene, 0.1, 0.1, 0.1)

last_time = glfw.get_time()

# Main loop
while not glfw.window_should_close(window):
    calculate_delta_time()

    if glfw.get_key(window, glfw.KEY_F5) == glfw.PRESS:
        hrl.HRL_DrawDebugCircle(scene, hrl.HRL_DEBUG_SOLID, 0.0, 0.0, 0.0, 30.0, 16, 1.0, 0.0, 1.0)

    hrl.HRL_BeginFrame()
    hrl.HRL_EndFrame()

    glfw.swap_buffers(window)
    glfw.poll_events()

    if dt > 0:
        print(f"FPS : {1/dt:.1f}")

    process_camera_movement(window)
    process_camera_rotation(window)
    hrl.HRL_SetCameraLocation(camera, cam_x, cam_y, cam_z)
    hrl.HRL_SetCameraRotation(camera, pitch, yaw, 0.0)
    hrl.HRL_SetLightLocation(light, cam_x, cam_y, cam_z)

    if glfw.get_key(window, glfw.KEY_ESCAPE) == glfw.PRESS:
        glfw.set_window_should_close(window, True)

    if glfw.get_key(window, glfw.KEY_F4) == glfw.PRESS:
        proj = hrl.FloatArray(16)
        hrl.HRL_GetProjectionMatrix(proj)
        for col in range(4):
            print(f"{proj[col*4+0]:.4f} {proj[col*4+1]:.4f} {proj[col*4+2]:.4f} {proj[col*4+3]:.4f}")

hrl.HRL_Shutdown()
glfw.terminate()
