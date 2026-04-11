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
	//16 bytes {
	//4 bytes
	uint32_t type;
	//4 bytes
	float intensity;
	//4 bytes
	float attenuation;
	//4 bytes
	float padding1;
	// }

	//16 bytes {
	//12 bytes
	glm::vec3 position;
	//4 bytes
	float padding2;
	// }

	//16 bytes {
	//12 bytes
	glm::vec3 rotation;
	//4 bytes
	float padding3;
	// }

	//16 bytes {
	//12 bytes
	glm::vec3 color;
	//4 bytes
	float padding4;
	// }
}GL_Light;

typedef struct {
	glm::mat4 model;
	glm::vec4 region;
}GL_SpriteInstance;

typedef struct {
	GL_SpriteInstance* instances;
	int instance_count;
	HRL_id mat;
}GL_RenderBatch;

typedef struct {
	GLuint fbo;
	GLuint texture;
	int width, height;
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
#define METALIC_INT (4)
#define ALPHA_INT (5)
inline const char* tex_uniform_name[6]
{
	"T_Albedo", "T_Normal", "T_Specular", "T_Roughness", "T_Metalic", "T_Alpha"
};

#endif