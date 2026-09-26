#ifndef GL33_DEFINITIONS_H
#define GL33_DEFINITIONS_H

#include <glad/glad.h>
#include <glm/glm.hpp>

class GL33_Texture;
class GL33_Shader;

//light
#define MAX_LIGHTS      32
/**
 * La norme std 140 de opengl a respecter pour les ubo demande d'alligner les objets
 * sur des multiples de 16, donc on ajoute des paddings pour correspondre
 */
typedef struct {
	//16 bytes
	uint32_t type;
	float intensity;
	float attenuation;
	float innerCutoff;

	//16 bytes
	glm::vec3 position;
	float outerCutoff;

	//16 bytes
	glm::vec3 rotation;
	float padding3;

	//16 bytes
	glm::vec3 color;
	float padding4;

	//64 bytes - world -> shadow texture coordinates for 2D shadow maps.
	glm::mat4 shadowMatrix;

	//16 bytes
	// x=bias, y=far plane, z=slot, w=type (0 none, 1 2D, 2 cubemap)
	glm::vec4 shadowParams;
}GL_Light;

static_assert(sizeof(GL_Light) == 144, "GL_Light must match the std140 Light layout");

typedef struct {
	GLuint fbo;
	// Scene color, bloom, object picking, and GI G-buffer attachments.
	// textures[3] = linear diffuse albedo, textures[4] = world-space normal.
	GLuint textures[5]{};
	GLuint depth_rbo = 0;

	// Optional multisample render target. The regular fbo/textures remain the
	// single-sample resolve target used by post-processing and presentation.
	GLuint msaa_fbo = 0;
	// MSAA counterparts for the five scene color attachments.
	GLuint msaa_textures[5]{};
	GLuint msaa_depth_rbo = 0;
	int msaa_samples = 1;

	int width = 0, height = 0;
}GL_Scene;



//used by EBO to render triangle without duplicating vertices
static unsigned int quad_indices[] = {
	0, 1, 2,
	2, 3, 0
};

static const float fullscreen_quad_verts[16] = {
	//x     y      u     v
	-1.f,  -1.f,   0.f,  0.f,
	 1.f,  -1.f,   1.f,  0.f,
	 1.f,   1.f,   1.f,  1.f,
	-1.f,   1.f,   0.f,  1.f,
};



#define ALBEDO_INT (0)
#define NORMAL_INT (1)
#define SPECULAR_INT (2)
#define ROUGHNESS_INT (3)
#define METALLIC_INT (4)
#define ALPHA_INT (5)
inline const char* tex_uniform_name[6]
{
	"T_Albedo", "T_Normal", "T_Specular", "T_Roughness", "T_Metallic", "T_Alpha"
};

#endif