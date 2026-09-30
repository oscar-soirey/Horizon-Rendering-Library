#ifndef HRL_TRANSFORM_SOURCE_H
#define HRL_TRANSFORM_SOURCE_H

#include "object_types.h"
#include <glm/gtc/matrix_transform.hpp>

HRL_Context* GetPrivateContext();

struct HRL_ResolvedTransform {
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;
};

static inline HRL_ResolvedTransform HRL_ResolveTransform(
    HRL_TransformHandle handle,
    const glm::vec3& fallbackPosition,
    const glm::vec3& fallbackRotation,
    const glm::vec3& fallbackScale)
{
    HRL_ResolvedTransform out{fallbackPosition, fallbackRotation, fallbackScale};
    if (handle == HRL_INVALID_ID)
        return out;

    HRL_Context* context = GetPrivateContext();
    if (!context)
        return out;

    auto it = context->transform_handles.find(handle);
    if (it == context->transform_handles.end())
        return out;

    const HRL_TransformHandleData& source = it->second;
    if (source.position)
        out.position = glm::vec3(source.position[0], source.position[1], source.position[2]);
    if (source.rotation_euler)
        out.rotation = glm::vec3(source.rotation_euler[0], source.rotation_euler[1], source.rotation_euler[2]);
    if (source.scale)
        out.scale = glm::vec3(source.scale[0], source.scale[1], source.scale[2]);
    return out;
}

static inline HRL_ResolvedTransform HRL_ResolveMeshTransform(const HRL_Mesh* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, object->scale_)
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveMeshInstanceTransform(const HRL_MeshInstance* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f))
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveLandscapeTransform(const HRL_Landscape* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, object->scale_)
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveLightTransform(const HRL_Light* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, glm::vec3(1.f))
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveCameraTransform(const HRL_Camera* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, glm::vec3(1.f))
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveVFXSystemTransform(const HRL_VFXSystem* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, object->scale_)
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveVFXEmitterTransform(const HRL_VFXEmitter* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, glm::vec3(1.f))
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveVolumetricFogTransform(const HRL_VolumetricFog* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position, glm::vec3(0.f), glm::vec3(1.f))
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveDecalTransform(const HRL_Decal* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position, object->rotation, object->size)
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline HRL_ResolvedTransform HRL_ResolveGizmoTransform(const HRL_Gizmo* object)
{
    return object
        ? HRL_ResolveTransform(object->transform_handle_, object->position_, object->rotation_, object->scale_)
        : HRL_ResolvedTransform{glm::vec3(0.f), glm::vec3(0.f), glm::vec3(1.f)};
}

static inline glm::mat4 HRL_MakeTRS(const HRL_ResolvedTransform& transform)
{
    glm::mat4 model(1.f);
    model = glm::translate(model, transform.position);
    model = glm::rotate(model, glm::radians(transform.rotation.x), glm::vec3(1.f, 0.f, 0.f));
    model = glm::rotate(model, glm::radians(transform.rotation.y), glm::vec3(0.f, 1.f, 0.f));
    model = glm::rotate(model, glm::radians(transform.rotation.z), glm::vec3(0.f, 0.f, 1.f));
    model = glm::scale(model, transform.scale);
    return model;
}

#endif
