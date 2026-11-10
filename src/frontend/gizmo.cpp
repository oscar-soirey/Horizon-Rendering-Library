// HRL frontend - gizmos.
#include "frontend_internal.h"

// ============================================================================
// GIZMOS
// ============================================================================

static HRL_Gizmo* FindGizmo(HRL_id id)
{
    auto it = ctx_.gizmos.find(id);
    return it == ctx_.gizmos.end() ? nullptr : it->second;
}

static const HRL_Gizmo* FindGizmoConst(HRL_id id)
{
    auto it = ctx_.gizmos.find(id);
    return it == ctx_.gizmos.end() ? nullptr : it->second;
}

static glm::mat4 GizmoProjection(const HRL_Viewport* viewport)
{
    if (!viewport || !viewport->camera_)
        return glm::mat4(1.f);
    const float width = std::max(1.f, static_cast<float>(GetWindowWidth()) * viewport->width_);
    const float height = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    const float aspect = width / height;
    if (viewport->camera_->type_ == HRL_PERSPECTIVE)
        return glm::perspective(glm::radians(viewport->camera_->value_), aspect,
            viewport->camera_->near_plane_, viewport->camera_->far_plane_);
    const float halfHeight = viewport->camera_->value_ * 0.5f;
    const float halfWidth = halfHeight * aspect;
    return glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
        viewport->camera_->near_plane_, viewport->camera_->far_plane_);
}

static glm::mat4 GizmoView(const HRL_Viewport* viewport)
{
    if (!viewport || !viewport->camera_)
        return glm::mat4(1.f);
    const HRL_Camera* cam = viewport->camera_;
    return glm::lookAt(cam->position_, cam->position_ + GetForwardVector(cam->rotation_), GetUpVector(cam->rotation_));
}

static bool GizmoScreenRay(const HRL_Viewport* viewport, float mouseX, float mouseY,
    glm::vec3& origin, glm::vec3& direction)
{
    if (!viewport || !viewport->camera_)
        return false;
    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    if (winW <= 1.f || winH <= 1.f)
        return false;
    const float vx = viewport->x_ * winW;
    const float vy = viewport->y_ * winH;
    const float vw = std::max(1.f, viewport->width_ * winW);
    const float vh = std::max(1.f, viewport->height_ * winH);
    if (mouseX < vx || mouseX > vx + vw || mouseY < vy || mouseY > vy + vh)
        return false;

    const float u = (mouseX - vx) / vw;
    const float v = (mouseY - vy) / vh;
    const float ndcX = u * 2.f - 1.f;
    const float ndcY = 1.f - v * 2.f;

    const glm::mat4 vp = GizmoProjection(viewport) * GizmoView(viewport);
    const glm::mat4 inv = glm::inverse(vp);
    glm::vec4 nearP = inv * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    glm::vec4 farP  = inv * glm::vec4(ndcX, ndcY,  1.f, 1.f);
    if (std::abs(nearP.w) < 1e-6f || std::abs(farP.w) < 1e-6f)
        return false;
    nearP /= nearP.w;
    farP /= farP.w;
    origin = glm::vec3(nearP);
    direction = glm::normalize(glm::vec3(farP - nearP));
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
           std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z);
}

static float GizmoWorldSize(const HRL_Gizmo* gizmo, const HRL_Viewport* viewport)
{
    if (!gizmo || !viewport || !viewport->camera_)
        return 1.f;
    if (!gizmo->use_screen_size_)
        return std::max(0.0001f, gizmo->world_size_);

    const glm::vec3 viewSpace = glm::vec3(GizmoView(viewport) * glm::vec4(gizmo->position_, 1.f));
    const float depth = std::max(0.001f, std::abs(viewSpace.z));
    const float viewportHeight = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    if (viewport->camera_->type_ == HRL_PERSPECTIVE) {
        const float worldHeight = 2.f * depth * std::tan(glm::radians(viewport->camera_->value_) * 0.5f);
        return std::max(0.0001f, gizmo->screen_size_pixels_ * worldHeight / viewportHeight);
    }
    return std::max(0.0001f, gizmo->screen_size_pixels_ * viewport->camera_->value_ / viewportHeight);
}

static glm::vec3 GizmoAxisVector(const HRL_Gizmo* gizmo, int axis)
{
    glm::vec3 local(0.f);
    if (axis == 0) local.x = 1.f;
    if (axis == 1) local.y = 1.f;
    if (axis == 2) local.z = 1.f;
    if (gizmo->space_ == HRL_GIZMO_SPACE_WORLD)
        return local;
    glm::mat4 r(1.f);
    r = glm::rotate(r, glm::radians(gizmo->rotation_.x), glm::vec3(1,0,0));
    r = glm::rotate(r, glm::radians(gizmo->rotation_.y), glm::vec3(0,1,0));
    r = glm::rotate(r, glm::radians(gizmo->rotation_.z), glm::vec3(0,0,1));
    return glm::normalize(glm::vec3(r * glm::vec4(local, 0.f)));
}

static float RaySegmentDistance(const glm::vec3& ro, const glm::vec3& rd,
    const glm::vec3& a, const glm::vec3& b, float* outSegT = nullptr, float* outRayT = nullptr)
{
    const glm::vec3 u = rd;
    const glm::vec3 v = b - a;
    const glm::vec3 w = ro - a;
    const float a0 = glm::dot(u,u);
    const float b0 = glm::dot(u,v);
    const float c0 = glm::dot(v,v);
    const float d0 = glm::dot(u,w);
    const float e0 = glm::dot(v,w);
    const float denom = a0*c0 - b0*b0;
    float sc, tc;
    if (denom < 1e-8f) {
        sc = 0.f;
        tc = c0 > 1e-8f ? glm::clamp(e0 / c0, 0.f, 1.f) : 0.f;
    } else {
        sc = (b0*e0 - c0*d0) / denom;
        tc = (a0*e0 - b0*d0) / denom;
        if (sc < 0.f) { sc = 0.f; tc = glm::clamp(e0 / c0, 0.f, 1.f); }
        else if (tc < 0.f) { tc = 0.f; sc = glm::max(0.f, -d0 / a0); }
        else if (tc > 1.f) { tc = 1.f; sc = glm::max(0.f, (b0 - d0) / a0); }
    }
    if (outSegT) *outSegT = tc;
    if (outRayT) *outRayT = sc;
    const glm::vec3 p = ro + sc*u;
    const glm::vec3 q = a + tc*v;
    return glm::length(p-q);
}

static bool RayPlaneIntersection(const glm::vec3& ro, const glm::vec3& rd,
    const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
{
    const float denom = glm::dot(rd, normal);
    if (std::abs(denom) < 1e-6f)
        return false;
    const float t = glm::dot(point-ro, normal) / denom;
    if (t < 0.f)
        return false;
    hit = ro + rd*t;
    return true;
}

struct GizmoPickResult {
    HRL_EGizmoPart part = HRL_GIZMO_PART_NONE;
    HRL_EGizmoOperation operation = HRL_GIZMO_OPERATION_NONE;
    float score = 1e30f;
};

static bool GizmoOperationVisible(const HRL_Gizmo* gizmo, HRL_EGizmoOperation op)
{
    if (!gizmo) return false;
    if ((static_cast<int>(gizmo->mode_) & static_cast<int>(op)) == 0) return false;
    if (op == HRL_GIZMO_OPERATION_TRANSLATE) return gizmo->show_translate_;
    if (op == HRL_GIZMO_OPERATION_ROTATE) return gizmo->show_rotate_;
    if (op == HRL_GIZMO_OPERATION_SCALE) return gizmo->show_scale_;
    return false;
}

static int GizmoOperationAxisMask(const HRL_Gizmo* gizmo, HRL_EGizmoOperation op)
{
    if (op == HRL_GIZMO_OPERATION_TRANSLATE) return gizmo->translate_axes_;
    if (op == HRL_GIZMO_OPERATION_ROTATE) return gizmo->rotate_axes_;
    if (op == HRL_GIZMO_OPERATION_SCALE) return gizmo->scale_axes_;
    return 0;
}

// Rotation arcs deliberately use the positive neighboring axes as their endpoints:
// X rotation: +Y -> +Z
// Y rotation: +Z -> +X
// Z rotation: +X -> +Y
// This makes the three quarter-wheels meet on the same positive axis handles.
static glm::vec3 GizmoRotateBasisU(const HRL_Gizmo* gizmo, int axis)
{
    const int nextAxis = (axis + 1) % 3;
    return GizmoAxisVector(gizmo, nextAxis);
}

static float GizmoRotateArcStart(int /*axis*/)
{
    return 0.f;
}

static bool GizmoAngleOnArc(float angle, float start, float sweep)
{
    static constexpr float kTwoPi = 6.28318530717958647692f;
    angle = std::fmod(angle + kTwoPi, kTwoPi);
    start = std::fmod(start + kTwoPi, kTwoPi);
    const float delta = std::fmod(angle - start + kTwoPi, kTwoPi);
    return delta <= sweep + 1e-4f;
}

static GizmoPickResult PickGizmoPart(const HRL_Gizmo* gizmo, const HRL_Viewport* viewport, float mouseX, float mouseY)
{
    GizmoPickResult result;
    if (!gizmo || !viewport || !gizmo->visible_ || !gizmo->enabled_)
        return result;
    glm::vec3 ro, rd;
    if (!GizmoScreenRay(viewport, mouseX, mouseY, ro, rd))
        return result;
    const float size = GizmoWorldSize(gizmo, viewport);
    const float pickRadius = size * 0.12f;
    const glm::vec3 center = gizmo->position_;

    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_TRANSLATE)) {
        const float centerDist = glm::length(glm::cross(rd, center-ro));
        if (centerDist < size * 0.22f && glm::dot(center-ro, rd) > 0.f) {
            result.part = HRL_GIZMO_PART_CENTER;
            result.operation = HRL_GIZMO_OPERATION_TRANSLATE;
            result.score = centerDist;
        }
    }

    // Translation owns the shaft, while scale owns the square tip. This keeps both modes
    // independently usable when their visuals are shown together.
    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_TRANSLATE)) {
        const int mask = gizmo->translate_axes_;
        for (int axis=0; axis<3; ++axis) {
            if (!(mask & (1<<axis))) continue;
            const glm::vec3 dir = GizmoAxisVector(gizmo, axis);
            const glm::vec3 end = center + dir * size;
            float segT = 0.f, rayT = 0.f;
            const float d = RaySegmentDistance(ro, rd, center + dir*(size*0.12f), center + dir*(size*0.80f), &segT, &rayT);
            if (d < pickRadius && rayT > 0.f && rayT < result.score) {
                result.part = static_cast<HRL_EGizmoPart>(axis+1);
                result.operation = HRL_GIZMO_OPERATION_TRANSLATE;
                result.score = rayT;
            }
            (void)end;
        }
    }

    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_SCALE)) {
        const int mask = gizmo->scale_axes_;
        for (int axis=0; axis<3; ++axis) {
            if (!(mask & (1<<axis))) continue;
            const glm::vec3 dir = GizmoAxisVector(gizmo, axis);
            const glm::vec3 tip = center + dir * size;
            const float half = size * 0.14f;
            float segT = 0.f, rayT = 0.f;
            const float d = RaySegmentDistance(ro, rd, tip - dir*half*1.3f, tip + dir*half*0.15f, &segT, &rayT);
            if (d < pickRadius * 1.15f && rayT > 0.f && rayT < result.score) {
                result.part = static_cast<HRL_EGizmoPart>(axis+1);
                result.operation = HRL_GIZMO_OPERATION_SCALE;
                result.score = rayT;
            }
        }
    }

    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_ROTATE)) {
        const float sweep = glm::radians(glm::clamp(gizmo->rotate_arc_degrees_, 15.f, 170.f));
        for (int axis=0; axis<3; ++axis) {
            if (!(gizmo->rotate_axes_ & (1<<axis))) continue;
            const glm::vec3 n = GizmoAxisVector(gizmo, axis);
            glm::vec3 hit;
            if (!RayPlaneIntersection(ro, rd, center, n, hit)) continue;
            const glm::vec3 radial = hit - center;
            const float radialLen = glm::length(radial);
            const float radialError = std::abs(radialLen - size);
            if (radialLen < 1e-5f || radialError > size * 0.11f) continue;
            const glm::vec3 u = GizmoRotateBasisU(gizmo, axis);
            const glm::vec3 v = glm::normalize(glm::cross(n, u));
            const float angle = std::atan2(glm::dot(radial, v), glm::dot(radial, u));
            if (!GizmoAngleOnArc(angle, GizmoRotateArcStart(axis), sweep)) continue;
            if (radialError < result.score) {
                result.part = static_cast<HRL_EGizmoPart>(axis+1);
                result.operation = HRL_GIZMO_OPERATION_ROTATE;
                result.score = radialError;
            }
        }
    }
    return result;
}

static void GizmoNotify(HRL_Gizmo* gizmo)
{
    if (gizmo && gizmo->changed_callback_)
        gizmo->changed_callback_(gizmo->id_, gizmo->active_part_, gizmo->changed_user_data_);
}

static void GizmoBeginDrag(HRL_Gizmo* gizmo, const HRL_Viewport* viewport)
{
    if (!gizmo || !viewport || gizmo->hovered_part_ == HRL_GIZMO_PART_NONE || gizmo->hovered_operation_ == HRL_GIZMO_OPERATION_NONE)
        return;
    glm::vec3 ro, rd;
    if (!GizmoScreenRay(viewport, ctx_.mouseX, ctx_.mouseY, ro, rd)) return;

    gizmo->active_part_ = gizmo->hovered_part_;
    gizmo->active_operation_ = gizmo->hovered_operation_;
    gizmo->dragging_ = true;
    gizmo->drag_viewport_ = [&]() -> HRL_id {
        for (const auto& [id, vp] : ctx_.viewports) if (vp == viewport) return id;
        return HRL_INVALID_ID;
    }();
    gizmo->drag_start_position_ = gizmo->position_;
    gizmo->drag_start_rotation_ = gizmo->rotation_;
    gizmo->drag_start_scale_ = gizmo->scale_;
    const int axisIndex = static_cast<int>(gizmo->active_part_) - 1;
    const glm::vec3 viewDir = glm::normalize(viewport->camera_->position_ - gizmo->position_);

    if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_TRANSLATE) {
        if (gizmo->active_part_ == HRL_GIZMO_PART_CENTER) {
            gizmo->drag_plane_normal_ = glm::normalize(GetForwardVector(viewport->camera_->rotation_));
            glm::vec3 hit;
            if (!RayPlaneIntersection(ro, rd, gizmo->position_, gizmo->drag_plane_normal_, hit))
                gizmo->dragging_ = false;
            else
                gizmo->drag_start_vector_ = hit - gizmo->position_;
        } else {
            const glm::vec3 axis = GizmoAxisVector(gizmo, axisIndex);
            glm::vec3 normal = glm::cross(axis, viewDir);
            if (glm::length(normal) < 1e-4f) normal = glm::cross(axis, glm::vec3(0,1,0));
            gizmo->drag_plane_normal_ = glm::normalize(glm::cross(normal, axis));
            glm::vec3 hit;
            if (!RayPlaneIntersection(ro, rd, gizmo->position_, gizmo->drag_plane_normal_, hit))
                gizmo->dragging_ = false;
            else
                gizmo->drag_start_axis_value_ = glm::dot(hit - gizmo->position_, axis);
            gizmo->drag_start_axis_ = axis;
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_ROTATE) {
        const glm::vec3 axis = GizmoAxisVector(gizmo, axisIndex);
        glm::vec3 hit;
        if (!RayPlaneIntersection(ro, rd, gizmo->position_, axis, hit)) {
            gizmo->dragging_ = false;
        } else {
            const glm::vec3 start = hit - gizmo->position_;
            if (glm::length(start) < 1e-5f) gizmo->dragging_ = false;
            else {
                gizmo->drag_start_vector_ = glm::normalize(start);
                gizmo->drag_start_axis_ = axis;
                gizmo->drag_start_angle_ = 0.f;
            }
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_SCALE) {
        const glm::vec3 axis = GizmoAxisVector(gizmo, axisIndex);
        glm::vec3 normal = glm::cross(axis, viewDir);
        if (glm::length(normal) < 1e-4f) normal = glm::cross(axis, glm::vec3(0,1,0));
        gizmo->drag_plane_normal_ = glm::normalize(glm::cross(normal, axis));
        glm::vec3 hit;
        if (!RayPlaneIntersection(ro, rd, gizmo->position_, gizmo->drag_plane_normal_, hit))
            gizmo->dragging_ = false;
        else {
            gizmo->drag_start_axis_value_ = glm::dot(hit - gizmo->position_, axis);
            gizmo->drag_start_axis_ = axis;
        }
    }
}

static void GizmoUpdateDrag(HRL_Gizmo* gizmo, const HRL_Viewport* viewport)
{
    if (!gizmo || !viewport || !gizmo->dragging_ || gizmo->active_operation_ == HRL_GIZMO_OPERATION_NONE)
        return;
    glm::vec3 ro, rd;
    if (!GizmoScreenRay(viewport, ctx_.mouseX, ctx_.mouseY, ro, rd)) return;
    const int axisIndex = static_cast<int>(gizmo->active_part_) - 1;
    const float size = GizmoWorldSize(gizmo, viewport);

    if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_TRANSLATE) {
        glm::vec3 hit;
        if (gizmo->active_part_ == HRL_GIZMO_PART_CENTER) {
            if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_plane_normal_, hit))
                gizmo->position_ = gizmo->drag_start_position_ + (hit - gizmo->drag_start_position_) - gizmo->drag_start_vector_;
        } else if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_plane_normal_, hit)) {
            const float axisValue = glm::dot(hit - gizmo->drag_start_position_, gizmo->drag_start_axis_);
            gizmo->position_ = gizmo->drag_start_position_ + gizmo->drag_start_axis_ * (axisValue - gizmo->drag_start_axis_value_);
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_ROTATE) {
        glm::vec3 hit;
        if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_start_axis_, hit)) {
            const glm::vec3 currentVector = hit - gizmo->position_;
            if (glm::length(currentVector) > 1e-5f) {
                const glm::vec3 current = glm::normalize(currentVector);
                const float angle = std::atan2(glm::dot(glm::cross(gizmo->drag_start_vector_, current), gizmo->drag_start_axis_),
                                               glm::dot(gizmo->drag_start_vector_, current));
                gizmo->rotation_ = gizmo->drag_start_rotation_;
                if (axisIndex == 0) gizmo->rotation_.x += glm::degrees(angle);
                if (axisIndex == 1) gizmo->rotation_.y += glm::degrees(angle);
                if (axisIndex == 2) gizmo->rotation_.z += glm::degrees(angle);
            }
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_SCALE) {
        glm::vec3 hit;
        if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_plane_normal_, hit)) {
            const float axisValue = glm::dot(hit - gizmo->drag_start_position_, gizmo->drag_start_axis_);
            const float delta = axisValue - gizmo->drag_start_axis_value_;
            const float factor = std::max(0.01f, 1.f + delta / std::max(size, 1e-4f));
            gizmo->scale_ = gizmo->drag_start_scale_;
            if (axisIndex == 0) gizmo->scale_.x *= factor;
            if (axisIndex == 1) gizmo->scale_.y *= factor;
            if (axisIndex == 2) gizmo->scale_.z *= factor;
        }
    }
    GizmoNotify(gizmo);
}

static void UpdateAllGizmoHover()
{
    for (auto& [id, gizmo] : ctx_.gizmos) {
        (void)id;
        if (!gizmo) continue;
        GizmoPickResult pick;
        auto vpIt = ctx_.viewports.find(gizmo->viewport_);
        if (vpIt != ctx_.viewports.end())
            pick = PickGizmoPart(gizmo, vpIt->second, ctx_.mouseX, ctx_.mouseY);
        gizmo->hovered_part_ = pick.part;
        gizmo->hovered_operation_ = pick.operation;
    }
}

void ProcessGizmoInput()
{
    if (ctx_.mouseLeftPressed && ctx_.mouseCaptureGizmo == HRL_INVALID_ID) {
        UpdateAllGizmoHover();
        HRL_id chosen = HRL_INVALID_ID;
        GizmoPickResult chosenPick;
        for (const auto& [id, gizmo] : ctx_.gizmos) {
            if (!gizmo || !gizmo->enabled_ || !gizmo->visible_ || gizmo->hovered_part_ == HRL_GIZMO_PART_NONE) continue;
            auto vpIt = ctx_.viewports.find(gizmo->viewport_);
            if (vpIt == ctx_.viewports.end()) continue;
            glm::vec3 ro, rd;
            if (!GizmoScreenRay(vpIt->second, ctx_.mouseX, ctx_.mouseY, ro, rd)) continue;
            const float distance = glm::length(gizmo->position_ - ro);
            if (distance < chosenPick.score) {
                chosenPick.part = gizmo->hovered_part_;
                chosenPick.operation = gizmo->hovered_operation_;
                chosenPick.score = distance;
                chosen = id;
            }
        }
        if (chosen != HRL_INVALID_ID) {
            ctx_.mouseCaptureGizmo = chosen;
            ctx_.mouseCaptureGizmoPart = chosenPick.part;
            HRL_Gizmo* gizmo = FindGizmo(chosen);
            auto vpIt = ctx_.viewports.find(gizmo->viewport_);
            if (vpIt != ctx_.viewports.end()) GizmoBeginDrag(gizmo, vpIt->second);
        }
    }

    if (ctx_.mouseCaptureGizmo != HRL_INVALID_ID) {
        HRL_Gizmo* gizmo = FindGizmo(ctx_.mouseCaptureGizmo);
        if (!gizmo) {
            ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
            ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
        } else {
            auto vpIt = ctx_.viewports.find(gizmo->viewport_);
            if (ctx_.mouseLeftDown && vpIt != ctx_.viewports.end()) GizmoUpdateDrag(gizmo, vpIt->second);
            if (ctx_.mouseLeftReleased) {
                gizmo->dragging_ = false;
                gizmo->active_part_ = HRL_GIZMO_PART_NONE;
                gizmo->active_operation_ = HRL_GIZMO_OPERATION_NONE;
                ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
                ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
            }
        }
    } else {
        UpdateAllGizmoHover();
    }
}


HRL_id HRL_CreateGizmo(HRL_id viewportid)
{
    auto vpIt = ctx_.viewports.find(viewportid);
    if (vpIt == ctx_.viewports.end() || !vpIt->second) {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateGizmo: invalid viewport ID");
        return HRL_INVALID_ID;
    }
    auto sceneIt = ctx_.scenes.find(vpIt->second->scene_);
    if (sceneIt == ctx_.scenes.end()) {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateGizmo: viewport scene is invalid");
        return HRL_INVALID_ID;
    }
    auto* gizmo = new (std::nothrow) HRL_Gizmo();
    if (!gizmo) {
        SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateGizmo: failed to allocate gizmo");
        return HRL_INVALID_ID;
    }
    const HRL_id id = GenerateHRL_ID();
    gizmo->id_ = id;
    gizmo->scene_ = vpIt->second->scene_;
    gizmo->viewport_ = viewportid;
    sceneIt->second->gizmos.emplace(id, gizmo);
    ctx_.gizmos.emplace(id, gizmo);
    return id;
}

void HRL_DeleteGizmo(HRL_id gizmoid)
{
    auto it = ctx_.gizmos.find(gizmoid);
    if (it == ctx_.gizmos.end()) {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteGizmo: invalid ID");
        return;
    }
    HRL_Gizmo* gizmo = it->second;
    if (ctx_.mouseCaptureGizmo == gizmoid) {
        ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
        ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
    }
    auto sceneIt = ctx_.scenes.find(gizmo->scene_);
    if (sceneIt != ctx_.scenes.end()) sceneIt->second->gizmos.erase(gizmoid);
    delete gizmo;
    ctx_.gizmos.erase(it);
}

int HRL_IsValidGizmo(HRL_id gizmoid) { return FindGizmoConst(gizmoid) ? HRL_TRUE : HRL_FALSE; }

void HRL_SetGizmoPosition(HRL_id id, float x, float y, float z) { if (auto* g=FindGizmo(id)) g->position_={x,y,z}; else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGizmoPosition: invalid ID"); }
void HRL_GetGizmoPosition(HRL_id id, float* x, float* y, float* z) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoPosition: invalid ID"); return;} if(x)*x=g->position_.x; if(y)*y=g->position_.y; if(z)*z=g->position_.z; }
void HRL_SetGizmoRotation(HRL_id id, float p, float y, float r) { if(auto* g=FindGizmo(id)) g->rotation_={p,y,r}; else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotation: invalid ID"); }
void HRL_GetGizmoRotation(HRL_id id, float* p, float* y, float* r) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoRotation: invalid ID"); return;} if(p)*p=g->rotation_.x; if(y)*y=g->rotation_.y; if(r)*r=g->rotation_.z; }
void HRL_SetGizmoScale(HRL_id id, float x, float y, float z) { if(auto* g=FindGizmo(id)) g->scale_={x,y,z}; else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScale: invalid ID"); }
void HRL_GetGizmoScale(HRL_id id, float* x, float* y, float* z) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoScale: invalid ID"); return;} if(x)*x=g->scale_.x; if(y)*y=g->scale_.y; if(z)*z=g->scale_.z; }
void HRL_SetGizmoMode(HRL_id id, HRL_EGizmoMode mode) { auto* g=FindGizmo(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoMode: invalid ID"); return;} const int allowed=HRL_GIZMO_MODE_TRANSLATE|HRL_GIZMO_MODE_ROTATE|HRL_GIZMO_MODE_SCALE; const int value=static_cast<int>(mode); if((value&~allowed)!=0 || value==0){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetGizmoMode: invalid mode flags"); return;} g->mode_=mode; }
HRL_EGizmoMode HRL_GetGizmoMode(HRL_id id) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoMode: invalid ID"); return HRL_GIZMO_MODE_TRANSLATE;} return g->mode_; }
void HRL_SetGizmoSpace(HRL_id id, HRL_EGizmoSpace space) { auto* g=FindGizmo(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoSpace: invalid ID"); return;} if(space<HRL_GIZMO_SPACE_WORLD||space>HRL_GIZMO_SPACE_LOCAL){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetGizmoSpace: invalid space"); return;} g->space_=space; }
HRL_EGizmoSpace HRL_GetGizmoSpace(HRL_id id) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoSpace: invalid ID"); return HRL_GIZMO_SPACE_WORLD;} return g->space_; }
void HRL_SetGizmoTranslateVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->show_translate_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoTranslateVisible: invalid ID");}
void HRL_SetGizmoRotateVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->show_rotate_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateVisible: invalid ID");}
void HRL_SetGizmoScaleVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->show_scale_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScaleVisible: invalid ID");}
void HRL_SetGizmoTranslateAxes(HRL_id id,int mask){if(auto*g=FindGizmo(id))g->translate_axes_=mask&7;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoTranslateAxes: invalid ID");}
void HRL_SetGizmoRotateAxes(HRL_id id,int mask){if(auto*g=FindGizmo(id))g->rotate_axes_=mask&7;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateAxes: invalid ID");}
void HRL_SetGizmoScaleAxes(HRL_id id,int mask){if(auto*g=FindGizmo(id))g->scale_axes_=mask&7;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScaleAxes: invalid ID");}
void HRL_SetGizmoSize(HRL_id id,float size){if(auto*g=FindGizmo(id)){if(size<=0||!std::isfinite(size)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetGizmoSize: size must be > 0");return;}g->world_size_=size;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoSize: invalid ID");}
void HRL_SetGizmoScreenSize(HRL_id id,float px){if(auto*g=FindGizmo(id)){if(px<=0||!std::isfinite(px)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetGizmoScreenSize: pixels must be > 0");return;}g->screen_size_pixels_=px;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScreenSize: invalid ID");}
void HRL_SetGizmoUseScreenSize(HRL_id id,int use){if(auto*g=FindGizmo(id))g->use_screen_size_=use!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoUseScreenSize: invalid ID");}
void HRL_SetGizmoVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->visible_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoVisible: invalid ID");}
void HRL_SetGizmoEnabled(HRL_id id,int e){if(auto*g=FindGizmo(id))g->enabled_=e!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoEnabled: invalid ID");}
void HRL_SetGizmoAxisColor(HRL_id id,int axis,float r,float g,float b,float a){auto* z=FindGizmo(id);if(!z){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoAxisColor: invalid ID");return;}if(axis!=HRL_GIZMO_AXIS_X&&axis!=HRL_GIZMO_AXIS_Y&&axis!=HRL_GIZMO_AXIS_Z){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetGizmoAxisColor: axis must be X, Y or Z");return;}int idx=axis==HRL_GIZMO_AXIS_X?0:(axis==HRL_GIZMO_AXIS_Y?1:2);z->axis_colors_[idx]=glm::clamp(glm::vec4(r,g,b,a),glm::vec4(0.f),glm::vec4(1.f));}
void HRL_SetGizmoCenterColor(HRL_id id,float r,float g,float b,float a){if(auto* gizmo=FindGizmo(id))gizmo->center_color_=glm::clamp(glm::vec4(r,g,b,a),glm::vec4(0.f),glm::vec4(1.f));else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoCenterColor: invalid ID");}
void HRL_SetGizmoHoverColor(HRL_id id,float r,float g,float b,float a){if(auto* gizmo=FindGizmo(id))gizmo->hover_color_=glm::clamp(glm::vec4(r,g,b,a),glm::vec4(0.f),glm::vec4(1.f));else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoHoverColor: invalid ID");}
HRL_EGizmoPart HRL_GetGizmoHoveredPart(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoHoveredPart: invalid ID");return HRL_GIZMO_PART_NONE;}return g->hovered_part_;}
HRL_EGizmoPart HRL_GetGizmoActivePart(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoActivePart: invalid ID");return HRL_GIZMO_PART_NONE;}return g->active_part_;}
HRL_EGizmoOperation HRL_GetGizmoHoveredOperation(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoHoveredOperation: invalid ID");return HRL_GIZMO_OPERATION_NONE;}return g->hovered_operation_;}
HRL_EGizmoOperation HRL_GetGizmoActiveOperation(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoActiveOperation: invalid ID");return HRL_GIZMO_OPERATION_NONE;}return g->active_operation_;}
void HRL_SetGizmoRotateArcDegrees(HRL_id id,float degrees){auto*g=FindGizmo(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateArcDegrees: invalid ID");return;}if(!std::isfinite(degrees)||degrees<=0.f||degrees>170.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateArcDegrees: degrees must be in (0,170]");return;}g->rotate_arc_degrees_=degrees;}
void HRL_SetGizmoChangedCallback(HRL_id id, HRL_CGizmoChanged cb, void* ud){auto*g=FindGizmo(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoChangedCallback: invalid ID");return;}g->changed_callback_=cb;g->changed_user_data_=ud;}

//EFFECTS
//BLOOM
