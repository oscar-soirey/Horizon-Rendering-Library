#include "gl33_renderer.h"

#include "../../hrl_gl.h"

#include "gl33_definitions.h"
#include "gl33_shader.h"
#include "gl33_texture.h"
#include "../../ressources/ressources.h"
#include "../../core/utils_functions.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

//HRL PRIVATE
extern HRL_Context* GetPrivateContext();


//UTILS
static void InitTextureAndBindToFBO(GLuint _texture, GLuint _fbo, int width, int height);

static void BindMaterial(HRL_Material* mat);
static void BatchSprites(const std::unordered_map<HRL_id, HRL_Mesh*>&, std::vector<GL_RenderBatch>& batches);

static glm::mat4 CalculateProjectionMatrix();
static glm::mat4 CalculateViewMatrix();

static void DrawSprites(const std::vector<GL_RenderBatch>& render_batches);
static void DrawPostProcessQuad(GLuint src_texture, HRL_PostProcess* pp);



//GL33 BACKEND IMPLEMENTATION
#define BUFFER_QUAD			0
#define BUFFER_SPRITE		1
#define BUFFER_DEBUG		2
#define BUFFER_COUNT		3

#define UBO_LIGHTS			0
#define UBO_COUNT				1

struct GL33_Backend {
	GLuint vao[BUFFER_COUNT];
	GLuint vbo[BUFFER_COUNT];
	GLuint ebo[BUFFER_COUNT];
	//to pass the mat4 model to the instance (and not to every vertices)
	GLuint sprite_inst_model_vbo;

	GLuint ubo[UBO_COUNT];

	//contains the render technique of the scene
	std::unordered_map<HRL_id, GL_Scene*> gpu_scenes;

	//backend ressources
	std::unordered_map<HRL_id, GL33_Shader*> shaders;
	std::unordered_map<HRL_id, GL33_Texture*> textures;

	std::unordered_map<int, HRL_id> fallback_textures;

	//post process pass (ping pong method)
	GLuint post_fbo[2];
	GLuint post_textures[2];
};
static GL33_Backend* bck_;



//Render context, used and updated every frame
typedef struct {
	//render context
	HRL_Viewport* viewport;
	GL33_Shader* shader;

	//cached matrices
	glm::mat4 proj_mat;
	glm::mat4 view_mat;

	//fog
	hrl_fog_t* current_fog;

	//debug
	size_t current_debug_buffer_size=0;
} GL33_State;
static GL33_State* ctx_;


void GL33_Init()
{
	//empty with opengl
}

void GL33_InitContext(HRL_uint _width, HRL_uint _height, void *loader)
{
	if (!gladLoadGLLoader((GLADloadproc)loader))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "Failed to init GLAD, (loader error)");
		return;
	}

	//create backend
	bck_ = new GL33_Backend();
	ctx_ = new GL33_State();

	//Gen VAO, VBO and EBO
	glGenVertexArrays(BUFFER_COUNT, bck_->vao);
	glGenBuffers(BUFFER_COUNT, bck_->vbo);
	glGenBuffers(BUFFER_COUNT, bck_->ebo);
	glGenBuffers(1, &bck_->sprite_inst_model_vbo);
	glGenBuffers(UBO_COUNT, bck_->ubo);

	//INIT QUAD BUFFER (static draw)
	glBindVertexArray(bck_->vao[BUFFER_QUAD]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_QUAD]);
	//alloca buffer data with
	glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), fullscreen_quad_verts, GL_STATIC_DRAW);
	//layout(location=0) in vec2 apos
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	//layout(location=1) in vec2 auv
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
	glEnableVertexAttribArray(1);
	//EBO
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->ebo[BUFFER_QUAD]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);

	//GEN FBO & TEXTURES (post processing)
	glGenFramebuffers(2, bck_->post_fbo);
	glGenTextures(2, bck_->post_textures);
	InitTextureAndBindToFBO(bck_->post_textures[0], bck_->post_fbo[0], (int)_width, (int)_height);
	InitTextureAndBindToFBO(bck_->post_textures[1], bck_->post_fbo[1], (int)_width, (int)_height);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);  //reset fbo binding


	//INIT SPRITE BUFFER
	glBindVertexArray(bck_->vao[BUFFER_SPRITE]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_SPRITE]);
	//alloca buffer data (with no value)
	glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), nullptr, GL_DYNAMIC_DRAW); //(dynamic draw)
	//layout(location=0) in vec2 apos (creer un autre vbo pour ne plus passer les vertices à chaque frames)
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	//layout(location=1) in vec2 auv
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
	glEnableVertexAttribArray(1);
	//bind vbo instance sprite
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_inst_model_vbo);
	glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), nullptr, GL_DYNAMIC_DRAW); //(dynamic draw)
	//layout(location=2) in mat4 amodel (hold location 2,3,4 and 5 ; [see shader code])
	for (int i = 0; i < 4; i++)
	{
		glVertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float), (void*)(i * 4 * sizeof(float)));
		glEnableVertexAttribArray(2 + i);
		glVertexAttribDivisor(2 + i, 1);  //read by instance, not by vertex (instancing)
	}
	//EBO
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->ebo[BUFFER_SPRITE]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);

	//DEBUG
	glBindVertexArray(bck_->vao[BUFFER_DEBUG]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_DEBUG]);
	//alloca buffer data (with no value)
	glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
	//layout(location = 0) in vec3 apos
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	//layout(location = 1) in vec3 acolor
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
	glEnableVertexAttribArray(1);

	//RESET VAO BINDING
	glBindVertexArray(0);


	//SHADERS

	//DEFAULT POST PROCESS
	auto* default_post_process_shader = new GL33_Shader();
	default_post_process_shader->GL33_Create(
		(const char*)res_post_vert_glsl,
		res_post_vert_glsl_len,
		(const char*)res_post_frag_glsl,
		res_post_frag_glsl_len
	);
	bck_->shaders.emplace(HRL_DEFAULT_POST_PROCESS_SHADER, default_post_process_shader);

	//SPRITE SHADER
	auto* sprite_shader = new GL33_Shader();
	sprite_shader->GL33_Create(
		(const char*)res_sprite_vert_glsl,
		res_sprite_vert_glsl_len,
		(const char*)res_sprite_frag_glsl,
		res_sprite_frag_glsl_len);

	bck_->shaders.emplace(HRL_SPRITE_SHADER, sprite_shader);

	//DEBUG SHADER
	auto* debug_shader = new GL33_Shader();
	debug_shader->GL33_Create(
		(const char*)res_debug_vert_glsl,
		res_debug_vert_glsl_len,
		(const char*)res_debug_frag_glsl,
		res_debug_frag_glsl_len
	);
	bck_->shaders.emplace(HRL_DEBUG_SHADER, debug_shader);


	//FALLBACK TEXTURES
	bck_->fallback_textures[ALBEDO_INT] = GL33_CreateTexture((const char*)res_default_albedo_png, res_default_albedo_png_len);
	bck_->fallback_textures[NORMAL_INT] = GL33_CreateTexture((const char*)res_default_normal_png, res_default_normal_png_len);
	bck_->fallback_textures[SPECULAR_INT] = GL33_CreateTexture((const char*)res_default_specular_png, res_default_specular_png_len);
	bck_->fallback_textures[ROUGHNESS_INT] = GL33_CreateTexture((const char*)res_default_roughness_png, res_default_roughness_png_len);
	bck_->fallback_textures[METALIC_INT] = GL33_CreateTexture((const char*)res_default_metalic_png, res_default_metalic_png_len);
	bck_->fallback_textures[ALPHA_INT] = GL33_CreateTexture((const char*)res_default_alpha_png, res_default_alpha_png_len);

	assert(bck_->fallback_textures[ALBEDO_INT] != HRL_INVALID_ID && "Failed to load fallback albedo");
	assert(bck_->fallback_textures[NORMAL_INT] != HRL_INVALID_ID && "Failed to load fallback normal");
	assert(bck_->fallback_textures[SPECULAR_INT] != HRL_INVALID_ID && "Failed to load fallback specular");
	assert(bck_->fallback_textures[ROUGHNESS_INT] != HRL_INVALID_ID && "Failed to load fallback roughness");
	assert(bck_->fallback_textures[METALIC_INT] != HRL_INVALID_ID && "Failed to load fallback metalic");
	assert(bck_->fallback_textures[ALPHA_INT] != HRL_INVALID_ID && "Failed to load fallback alpha");


	//UBO
	//LIGHTS
	glBindBuffer(GL_UNIFORM_BUFFER, bck_->ubo[UBO_LIGHTS]);
	glBufferData(GL_UNIFORM_BUFFER, MAX_LIGHTS * sizeof(HRL_Light), nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, bck_->ubo[UBO_LIGHTS]);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GL33_Shutdown()
{
	for (const auto s : bck_->shaders)
	{
		delete s.second;
	}
	for (const auto t : bck_->textures)
	{
		delete t.second;
	}

	//DELETE POST PROCESS OBJECTS
	glDeleteFramebuffers(2, bck_->post_fbo);
	glDeleteTextures(2, bck_->post_textures);
	//DELETE BUFFERS
	glDeleteVertexArrays(BUFFER_COUNT, bck_->vao);
	glDeleteBuffers(BUFFER_COUNT, bck_->vbo);
	glDeleteBuffers(BUFFER_COUNT, bck_->ebo);

	delete bck_;
	delete ctx_;
}


void GL33_WindowResizeCallback(int width, int height)
{
	for (int i = 0; i < 2; i++)
	{
		glBindTexture(GL_TEXTURE_2D, bck_->post_textures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
}



void GL33_DrawScene(hrl_scene_t *scene, HRL_id scene_id)
{
	auto scene_it = bck_->gpu_scenes.find(scene_id);
	if (scene_it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33_DrawScene: tried to draw scene with invalid gpu ID");
		return;
	}

	GLuint scene_fbo = scene_it->second->fbo;

	ctx_->current_fog = &scene->fog;

	//clear scene once (évite les artefacts entre viewports)
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	for (const auto& v : scene->viewports)
	{
		//bind viewport to context
		ctx_->viewport = v.second;
		float winW = (float)GetWindowWidth();
		float winH = (float)GetWindowHeight();
		glViewport(
			(GLsizei)(v.second->x_ * winW),
			(GLsizei)(v.second->y_ * winH),
			(GLsizei)(v.second->width_ * winW),
			(GLsizei)(v.second->height_ * winH)
		);

		std::vector<GL_RenderBatch> render_batches;
		BatchSprites(scene->meshes, render_batches);

		ctx_->proj_mat = CalculateProjectionMatrix();
		ctx_->view_mat = CalculateViewMatrix();

		bool has_post_process = !v.second->post_processes.empty();

		// ---- STEP 1 : rendu de la scène ----
		// Si post-process : on rend dans post_fbo[0] (source du ping-pong)
		// Sinon           : on rend directement dans scene_fbo
		GLuint initial_target = has_post_process ? bck_->post_fbo[0] : scene_fbo;
		glBindFramebuffer(GL_FRAMEBUFFER, initial_target);
		if (has_post_process)
		{
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		DrawSprites(render_batches);

		// ---- STEP 2 : chaîne de post-process (ping-pong) ----
		if (has_post_process)
		{
			int src = 0;

			for (const auto& [priority, pp] : v.second->post_processes)
			{
				int dst = 1 - src;

				glBindFramebuffer(GL_FRAMEBUFFER, bck_->post_fbo[dst]);
				glClear(GL_COLOR_BUFFER_BIT);
				DrawPostProcessQuad(bck_->post_textures[src], pp);

				src = dst;
			}

			// ---- STEP 3 : blit direct vers scene_fbo ----
			glBindFramebuffer(GL_READ_FRAMEBUFFER, bck_->post_fbo[src]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_fbo);
			//GPU copy, zero overhead
			glBlitFramebuffer(
					0, 0, GetWindowWidth(), GetWindowHeight(),
					0, 0, GetWindowWidth(), GetWindowHeight(),
					GL_COLOR_BUFFER_BIT,
					GL_NEAREST
			);
		}
	}
}






//UTILS IMPLEMENTATION
static void InitTextureAndBindToFBO(GLuint _texture, GLuint _fbo, int width, int height)
{
	//on initialise avec les bonnes valeurs la texture
	glBindTexture(GL_TEXTURE_2D, _texture);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	//pour eviter les artefacts sur les bords
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	//on unbind la texture du container texture openGL (on va la bind une seule fois au FBO correspondant)
	glBindTexture(GL_TEXTURE_2D, 0);

	//on attache la texture au framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _texture, 0);
}



//MATERIALS
static void ApplyFallback(int index)
{
	//texture non trouvée, on passe la fallback texture
	glActiveTexture(GL_TEXTURE0 + index);
	HRL_id fallback_hrl_id = bck_->fallback_textures[index];
	glBindTexture(GL_TEXTURE_2D, bck_->textures[fallback_hrl_id]->GetGL_ID());
}
static void BindMaterial(HRL_Material* mat)
{
	auto it = bck_->shaders.find(mat->shader_);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_FATAL, "Bind material error : shader doesn't exists");
		return;
	}
	auto* s = it->second;
	//on set CurrentShader pour spécifier que les prochains calls utiliseront ce shader
	ctx_->shader = s;
	s->Use();

	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);
	s->SetVec3("CamPos", ctx_->viewport->camera_->position_);
	s->SetVec3("TintColor", glm::vec3(1.f));

	//fog uniforms
	s->SetInt("FogEnabled", ctx_->current_fog->enabled);
	s->SetInt("FogMode", ctx_->current_fog->mode);
	s->SetVec4("FogColor", {
		ctx_->current_fog->r,
		ctx_->current_fog->g,
		ctx_->current_fog->b,
		1.f}
	);
	s->SetFloat("FogStart", ctx_->current_fog->range_start);
	s->SetFloat("FogEnd", ctx_->current_fog->range_end);
	s->SetFloat("FogDensity", ctx_->current_fog->density);


	//on passe tous les uniforms donnés par l'utilisateur
	for (const auto& [name, value] : mat->intParams_)
	{
		s->SetInt(name, value);
	}

	//utilisé pour unbind les textures apres avoir draw
	//textureSlotsBinded = (int)mat->textureParams_.size();

	for (int i=0; i<6; i++)
	{
		//on passe toujours les memes uniforms
		s->SetInt(tex_uniform_name[i], i);

		//on recherche la texture
		auto itParam = mat->textureParams_.find(std::string(tex_uniform_name[i]));
		if (itParam == mat->textureParams_.end())
		{
			ApplyFallback(i);
			continue;
		}

		auto itTexture = bck_->textures.find(itParam->second);
		if (itTexture == bck_->textures.end())
		{
			ApplyFallback(i);
			continue;
		}

		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, itTexture->second->GetGL_ID());
	}

	//apply uniforms
	for (auto& [name, value] : mat->floatParams_)
	{
		s->SetFloat(name, value);
	}
	for (auto& [name, value] : mat->vec2Params_)
	{
		s->SetVec2(name, value);
	}
	for (auto& [name, value] : mat->vec3Params_)
	{
		s->SetVec3(name, value);
	}
	for (auto& [name, value] : mat->vec4Params_)
	{
		s->SetVec4(name, value);
	}
}


//BATCHING
static void BatchSprites(const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, std::vector<GL_RenderBatch>& batches)
{
	for (const auto& [id, mesh] : meshes)
	{
		if (mesh->type_ == HRL_SPRITE)
		{
			glm::mat4 model = glm::mat4(1.f);

			//aller à la position du mesh
			model = glm::translate(model, mesh->position_);

			//aller au pivot
			model = glm::translate(model, mesh->pivot_point_);

			//tourner
			model = glm::rotate(model, mesh->rotation_.x, glm::vec3(1.f, 0.f, 0.f));
			model = glm::rotate(model, mesh->rotation_.y, glm::vec3(0.f, 1.f, 0.f));
			model = glm::rotate(model, mesh->rotation_.z, glm::vec3(0.f, 0.f, 1.f));

			//revenir en arrière
			model = glm::translate(model, -mesh->pivot_point_);

			//scale
			model = glm::scale(model, mesh->scale_);

			auto* sprite = static_cast<HRL_MeshSprite*>(mesh);

			auto* inst = new GL_SpriteInstance(model, {sprite->region_[0], sprite->region_[1], sprite->region_[2], sprite->region_[3]});
			GL_RenderBatch batch{inst, 1, mesh->material_};
			batches.emplace_back(batch);

			//Trier par distance a la camera
			glm::vec3 camPos = ctx_->viewport->camera_->position_;

			std::sort(batches.begin(), batches.end(), [&](const GL_RenderBatch& a, const GL_RenderBatch& b) {
					// tu as besoin de la position du mesh dans le batch
					glm::vec3 posA = glm::vec3(a.instances[0].model[3]);
					glm::vec3 posB = glm::vec3(b.instances[0].model[3]);
					float distA = glm::distance(camPos, posA);
					float distB = glm::distance(camPos, posB);
					return distA > distB; //plus loin = dessiné en premier
			});
		}
	}
}

//RENDERING
static void DrawSprites(const std::vector<GL_RenderBatch>& render_batches)
{
	for (const auto& rb : render_batches)
	{
		auto it_mat = GetPrivateContext()->materials.find(rb.mat);
		if (it_mat == GetPrivateContext()->materials.end())
		{
			SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "DrawSprites: tried to bind material, invalid ID");
			return;
		}

		BindMaterial(it_mat->second);

		std::vector<float> vertices_data;
		std::vector<float> instance_data;

		for (int i = 0; i < rb.instance_count; i++)
		{
			auto inst = rb.instances[i];

			float uMin = inst.region[0];
			float vMin = inst.region[1];
			float uMax = inst.region[2];
			float vMax = inst.region[3];
			float vertices[] = {
				-0.5f, -0.5f,  uMin, vMin,
				 0.5f, -0.5f,  uMax, vMin,
				 0.5f,  0.5f,  uMax, vMax,
				-0.5f,  0.5f,  uMin, vMax
			};
			vertices_data.insert(vertices_data.end(), vertices, vertices + 16);

			float* model_p = glm::value_ptr(inst.model);
			instance_data.insert(instance_data.end(), model_p, model_p + 16);
		}

		glBindVertexArray(bck_->vao[BUFFER_SPRITE]);

		glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_SPRITE]);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertices_data.size() * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(vertices_data.size() * sizeof(float)), vertices_data.data());

		glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_inst_model_vbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(instance_data.size() * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(instance_data.size() * sizeof(float)), instance_data.data());

		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, rb.instance_count);
	}
}

static void DrawPostProcessQuad(GLuint src_texture, HRL_PostProcess* pp)
{
	auto mat_it = GetPrivateContext()->materials.find(pp->material_);
	if (mat_it == GetPrivateContext()->materials.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "DrawPostProcessQuad: invalid material ID");
		return;
	}
	HRL_Material* mat = mat_it->second;

	auto shader_it = bck_->shaders.find(mat->shader_);
	if (shader_it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "DrawPostProcessQuad: invalid shader ID");
		return;
	}
	GL33_Shader* shader = shader_it->second;
	shader->Use();

	//texture de la passe précédente (ou de la scène)
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_texture);
	shader->SetInt("uScene", 0);
	shader->SetVec2("uScreenSize",{ctx_->viewport->x_ * (float)GetWindowWidth(), ctx_->viewport->y_ * (float)GetWindowHeight()});

	//uniforms utilisateur (ex: saturation, brightness...)
	for (const auto& [name, value] : mat->floatParams_)
		shader->SetFloat(name, value);
	for (const auto& [name, value] : mat->intParams_)
		shader->SetInt(name, value);
	for (const auto& [name, value] : mat->vec2Params_)
		shader->SetVec2(name, value);
	for (const auto& [name, value] : mat->vec3Params_)
		shader->SetVec3(name, value);
	for (const auto& [name, value] : mat->vec4Params_)
		shader->SetVec4(name, value);

	glBindVertexArray(bck_->vao[BUFFER_QUAD]);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}


//EFFECTS
void GL33_FogPropertyChanged(HRL_id scene, hrl_fog_t* fog_ptr)
{

}



//MATRICES
static glm::mat4 CalculateProjectionMatrix()
{
	//taille absolue du viewport width et height (on prend en compte la taille de la fenetre et la taille relative du viewport HRL)
	float viewportWidth  = (float)GetWindowWidth() * ctx_->viewport->width_;
	float viewportHeight = (float)GetWindowHeight() * ctx_->viewport->height_;

	//on evite la division par 0
	if (viewportHeight < 1e-3f)
	{
		viewportHeight = 1.f;
	}

	//calcul du ratio largeur/hauteur du viewport
	float aspect = viewportWidth / viewportHeight;

	glm::mat4 proj;
	if (ctx_->viewport->camera_->type_ == HRL_PERSPECTIVE)
	{
		proj = glm::perspective(glm::radians(ctx_->viewport->camera_->value_), aspect, ctx_->viewport->camera_->near_plane_, ctx_->viewport->camera_->far_plane_);
	}
	else
	{
		//on calcule la taille en hauteur d'abord, puis on fait le calcul de la largeur en fonction de la hauteur et de l'aspect
		float halfHeight = ctx_->viewport->camera_->value_ * 0.5f;
		float halfWidth  = halfHeight * aspect;
		proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
											ctx_->viewport->camera_->near_plane_, ctx_->viewport->camera_->far_plane_);
	}
	return proj;
}
static glm::mat4 CalculateViewMatrix()
{
	//position et vue de la camera
	glm::mat4 view = glm::lookAt(
		ctx_->viewport->camera_->position_,
		ctx_->viewport->camera_->position_ + GetForwardVector(ctx_->viewport->camera_->rotation_),
		GetUpVector(ctx_->viewport->camera_->rotation_)
	);
	return view;
}



//LIGHTS
void GL33_UpdateLights(const std::vector<HRL_Light*>& _lights)
{
	//on commence par creer le tableau de GL_Lights
	GL_Light gpu_lights[MAX_LIGHTS];
	size_t count = 0;

	//on rempli le tableau
	for (const auto& light: _lights)
	{
		if (count >= MAX_LIGHTS)
		{
			break;
		}

		gpu_lights[count].type = light->type_;
		gpu_lights[count].intensity = light->intensity_;
		gpu_lights[count].attenuation = light->attenuation_;
		gpu_lights[count].padding1 = 0.f;

		gpu_lights[count].position = light->position_;
		gpu_lights[count].padding2 = 0.f;

		gpu_lights[count].rotation = light->rotation_;
		gpu_lights[count].padding3 = 0.f;

		gpu_lights[count].color = light->color_;
		gpu_lights[count].padding4 = 0.f;

		++count;
	}

	//on passe les données à opengl
	glBindBuffer(GL_UNIFORM_BUFFER, bck_->ubo[UBO_LIGHTS]);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)(count * sizeof(GL_Light)), gpu_lights);
}





//SCENES
void GL33_CreateScene(HRL_id _newSceneid, int _renderOnScreen)
{
	auto* scene = new GL_Scene();
	if (_renderOnScreen)
	{
		//le framebuffer est 0 (ecran)
		scene->fbo = 0;
	}
	else
	{
		glGenTextures(1, &scene->texture);
		glBindTexture(GL_TEXTURE_2D, scene->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

		scene->width = 512;
		scene->height = 512;

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glGenFramebuffers(1, &scene->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, scene->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene->texture, 0);

		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		assert(status == GL_FRAMEBUFFER_COMPLETE && "Framebuffer incomplete!");
	}

	//les HRL_id sont partagés entre le backend et l'api
	bck_->gpu_scenes.emplace(_newSceneid, scene);
}
void GL33_DeleteScene(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_DeleteScene error: invalid scene id");
		return;
	}

	if (it->second->fbo != 0)
	{
		//scene is not rendered at screen
		glDeleteTextures(1, &it->second->texture);
		glDeleteFramebuffers(1, &it->second->fbo);
	}
	delete it->second;
	bck_->gpu_scenes.erase(it);
}
void GL33_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_ResizeSceneTexture error: invalid scene id");
		return;
	}
	if (it->second->fbo == 0)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, "GL33_ResizeSceneTexture error: scene is render on the screen");
		return;
	}

	GLuint tex = it->second->texture;
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _width, _height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	it->second->width = _width;
	it->second->height = _height;
}






//CREATE & DELETE CUSTOM SHADERS
HRL_id GL33_CreateShader(const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
	//on crée le shader et on recupere le code d'erreur
	auto* s = new GL33_Shader();
	int error = s->GL33_Create(_vertContent, _vertSize, _fragContent, _fragSize);

	//si il n'y a pas d'erreur, on génere un ID et on push le shader dans la liste des shaders, sinon on retourne invalid
	//la classe shader s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
	if (error == 0)
	{
		HRL_id id = GenerateHRL_ID();
		bck_->shaders.emplace(id, s);
		return id;
	}
	return HRL_INVALID_ID;
}
void GL33_DeleteShader(HRL_id _id)
{
	auto it = bck_->shaders.find(_id);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteShader error : Shader ID doesn't exists");
		return;
	}
	delete it->second;
	bck_->shaders.erase(it);
}



//TEXTURES
HRL_id GL33_CreateTexture(const char* _imageContent, size_t _imageSize)
{
  //on crée la texture et on récupere le code d'erreur
  auto* t = new GL33_Texture();
  int error = t->GL33_Create(_imageContent, _imageSize);

  //si il n'y a pas d'erreur, on génere un ID et on push la texture dans la liste des textures, sinon on retourne invalid
  //la classe texture s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    bck_->textures.emplace(id, t);
    return id;
  }
  return HRL_INVALID_ID;
}
HRL_id GL33_CreateTextureFromBitmap(BitmapResult bmp)
{
  //on crée la texture et on récupere le code d'erreur
  auto* t = new GL33_Texture();
  int error = t->GL33_CreateFromBitmap(&bmp);

  //si il n'y a pas d'erreur, on génere un ID et on push la texture dans la liste des textures, sinon on retourne invalid
  //la classe texture s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    bck_->textures.emplace(id, t);
    return id;
  }
  return HRL_INVALID_ID;
}
void GL33_DeleteTexture(HRL_id _id)
{
  auto it = bck_->textures.find(_id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteTexture error : Texture ID doesn't exists");
    return;
  }
  delete it->second;
  bck_->textures.erase(it);
}
void GL33_GetTextureSize(HRL_id id, int *width, int *height)
{
  auto it = bck_->textures.find(id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_GetTextureSize error : Texture ID doesn't exists");
    return;
  }
  *width = (int)it->second->GetWidth();
  *height = (int)it->second->GetHeight();
}
void GL33_SetTextureMinFilter(HRL_id id, HRL_uint _filter)
{
  auto it = bck_->textures.find(id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_SetTextureMinFilter error : Texture ID doesn't exists");
    return;
  }
  it->second->SetMinFilter(_filter);
}
void GL33_SetTextureMaxFilter(HRL_id id, HRL_uint _filter)
{
  auto it = bck_->textures.find(id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_SetTextureMaxFilter error : Texture ID doesn't exists");
    return;
  }
  it->second->SetMaxFilter(_filter);
}



//POST PROCESSING
void GL33_CreatePostProcess(HRL_id post, int priority)
{

}
void GL33_DeletePostProcess(HRL_id post)
{

}
void GL33_ResetFramebuffer()
{

}





//HRL UTILS
void GL33_GetProjectionMatrix(float *aa)
{
	glm::mat4 proj = ctx_->proj_mat;
	memcpy(aa, glm::value_ptr(proj), sizeof(float) * 16);
}
void GL33_GetViewMatrix(float *aa)
{
	glm::mat4 proj = ctx_->view_mat;
	memcpy(aa, glm::value_ptr(proj), sizeof(float) * 16);
}
void GL33_GetModelMatrix(HRL_Mesh *mesh, float *aa)
{

}




//DEBUG
void GL33_DrawDebug(const DebugRenderer &_renderer, float line_thickness)
{
	auto it = bck_->shaders.find(HRL_DEBUG_SHADER);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "GL33_DrawDebug error : debug shader doesn't exists");
		return;
	}
	auto* s = it->second;
	//on set CurrentShader pour spécifier que les prochains calls utiliseront ce shader
	ctx_->shader = s;
	s->Use();

	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);

	//bind vao and initialize opengl evironement
	glBindVertexArray(bck_->vao[BUFFER_DEBUG]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_DEBUG]);

	glEnable(GL_DEPTH_TEST);

	//remplacer par une seule fonction dans la vtable pour eviter de le faire a chaque frames
	glLineWidth(line_thickness);

	// lignes
	if (!_renderer.lines.empty())
	{
		size_t size = _renderer.lines.size() * sizeof(DebugVertex);

		//réalloue si le buffer est trop petit
		if (size > ctx_->current_debug_buffer_size)
		{
			glBufferData(GL_ARRAY_BUFFER, (GLsizei)size, _renderer.lines.data(), GL_STREAM_DRAW);
			ctx_->current_debug_buffer_size = size;
		}
		else
		{
			glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizei)size, _renderer.lines.data());
		}

		glDrawArrays(GL_LINES, 0, (GLsizei)_renderer.lines.size());
	}

	// triangles
	if (!_renderer.triangles.empty())
	{
		size_t size = _renderer.triangles.size() * sizeof(DebugVertex);

		if (size > ctx_->current_debug_buffer_size)
		{
			glBufferData(GL_ARRAY_BUFFER, (GLsizei)size, _renderer.triangles.data(), GL_STREAM_DRAW);
			ctx_->current_debug_buffer_size = size;
		}
		else
		{
			glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizei)size, _renderer.triangles.data());
		}

		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_renderer.triangles.size());
	}
}




//HRL REQUESTS
int GL33_IsValidTexture(HRL_id tex)
{
	auto it = bck_->textures.find(tex);
	if (it == bck_->textures.end())
	{
		return 0;
	}
	return 1;
}
int GL33_IsValidShader(HRL_id shader)
{
	auto it = bck_->shaders.find(shader);
	if (it == bck_->shaders.end())
	{
		return 0;
	}
	return 1;
}



///////// HRL_GL (hrl_gl.h) /////////
unsigned int HRL_GL_GetTextureGL_ID(HRL_id _textureid)
{
	auto it = bck_->textures.find(_textureid);
	if (it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetTextureGL_ID error : Texture ID doesn't exists");
		return GL_INVALID_VALUE;
	}

	return it->second->GetGL_ID();
}
unsigned int HRL_GL_GetShaderGL_ID(HRL_id _shaderid)
{
	auto it = bck_->shaders.find(_shaderid);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetShaderGL_ID error : Shader ID doesn't exists");
		return GL_INVALID_VALUE;
	}

	return it->second->GetId();
}
unsigned int HRL_GL_GetSceneTextureGL_ID(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneTextureGL_ID : scene ID is not valid");
		return GL_INVALID_VALUE;
	}

	return it->second->texture;
}
