//! Renders a textured cube offscreen and saves a screenshot.
//!     cargo run --example cube -- out.png [texture.png]
use std::os::raw::c_char;

unsafe extern "C" fn on_error(code: hrl::HRL_EError, sev: hrl::HRL_ESeverity, detail: *const c_char) {
    let d = if detail.is_null() { String::new() } else { std::ffi::CStr::from_ptr(detail).to_string_lossy().into_owned() };
    eprintln!("[HRL {:?}] {:?}: {}", sev, code, d);
}

fn face(v: &mut Vec<hrl::HRL_Vertex3D>, idx: &mut Vec<hrl::HRL_uint>, n: [f32; 3], u: [f32; 3], w: [f32; 3]) {
    let b = v.len() as u32;
    for (cu, cv) in [(-1.0f32, -1.0f32), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
        let mut x = hrl::HRL_Vertex3D::default();
        for k in 0..3 {
            x.position[k] = 0.5 * (n[k] + cu * u[k] + cv * w[k]);
            x.normal[k] = n[k];
            x.tangent[k] = u[k];
            x.bitangent[k] = w[k];
        }
        x.uv = [cu * 0.5 + 0.5, cv * 0.5 + 0.5];
        v.push(x);
    }
    idx.extend_from_slice(&[b, b + 1, b + 2, b + 2, b + 3, b]);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let out = args.get(1).cloned().unwrap_or("cube_rust.png".into());
    unsafe { hrl::register_error_callback(Some(on_error)) };
    hrl::init(hrl::HRL_VULKAN);
    unsafe { hrl::init_context(320, 240, std::ptr::null_mut()) };
    let s = hrl::create_scene(0);
    let cam = hrl::create_camera(s, hrl::HRL_PERSPECTIVE);
    hrl::set_camera_location(cam, 0.0, 1.5, 5.0);
    hrl::set_camera_rotation(cam, -15.0, -90.0, 0.0);
    hrl::set_camera_perspective_fov(cam, 60.0);
    hrl::create_viewport(s, cam, 0.0, 0.0, 1.0, 1.0);
    hrl::set_sky_sphere_enabled(s, 1);
    let (mut v, mut i) = (Vec::new(), Vec::new());
    for (n, u, w) in [([1., 0., 0.], [0., 0., -1.], [0., 1., 0.]), ([-1., 0., 0.], [0., 0., 1.], [0., 1., 0.]),
                      ([0., 1., 0.], [1., 0., 0.], [0., 0., -1.]), ([0., -1., 0.], [1., 0., 0.], [0., 0., 1.]),
                      ([0., 0., 1.], [1., 0., 0.], [0., 1., 0.]), ([0., 0., -1.], [-1., 0., 0.], [0., 1., 0.])] {
        face(&mut v, &mut i, n, u, w);
    }
    let mesh = hrl::create_mesh3d(s, &v, &i);
    hrl::set_mesh_rotation(mesh, 0.0, 30.0, 0.0);
    let mat = hrl::create_material(hrl::HRL_MESH_3D_SHADER);
    hrl::material_set_vec3(mat, "TintColor", 0.3, 0.9, 0.4);
    if let Some(tex) = args.get(2) {
        let data = std::fs::read(tex).expect("texture");
        hrl::material_set_texture(mat, hrl::HRL_T_ALBEDO, hrl::create_texture(&data));
    }
    hrl::set_mesh_material(mesh, mat);
    let l = hrl::create_light(s, hrl::HRL_DIRECTIONAL_LIGHT);
    hrl::set_light_rotation(l, -50.0, 30.0, 0.0);
    hrl::set_light_intensity(l, 2.0);
    hrl::add_screen_message(s, 5.0, "Hello from Rust 100%");
    let (err, detail, sev) = hrl::get_last_error();
    println!("last error: {:?} {:?} {:?}", err, detail, sev);
    hrl::wait_for_all_async_resources();
    for f in 0..3 {
        hrl::begin_frame();
        if f == 2 { hrl::take_screenshot(s, &out); }
        hrl::end_frame();
    }
    hrl::shutdown();
    println!("DONE");
}
