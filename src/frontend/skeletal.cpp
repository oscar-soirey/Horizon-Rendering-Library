// HRL frontend - skeletal meshes and animation.
#include "frontend_internal.h"

static HRL_SkeletalBoneTransform HRLSkeletal_TransformFromUFBX(const ufbx_transform& transform)
{
	HRL_SkeletalBoneTransform result{};
	result.translation[0] = (float)transform.translation.x;
	result.translation[1] = (float)transform.translation.y;
	result.translation[2] = (float)transform.translation.z;
	result.rotation[0] = (float)transform.rotation.x;
	result.rotation[1] = (float)transform.rotation.y;
	result.rotation[2] = (float)transform.rotation.z;
	result.rotation[3] = (float)transform.rotation.w;
	result.scale[0] = (float)transform.scale.x;
	result.scale[1] = (float)transform.scale.y;
	result.scale[2] = (float)transform.scale.z;
	return result;
}

static glm::mat4 HRLSkeletal_TransformToMatrix(const HRL_SkeletalBoneTransform& transform)
{
	const glm::vec3 translation(transform.translation[0], transform.translation[1], transform.translation[2]);
	const glm::quat rotation(
		transform.rotation[3],
		transform.rotation[0],
		transform.rotation[1],
		transform.rotation[2]);
	const glm::vec3 scale(transform.scale[0], transform.scale[1], transform.scale[2]);
	glm::mat4 matrix(1.f);
	matrix = glm::translate(matrix, translation);
	matrix *= glm::mat4_cast(rotation);
	matrix = glm::scale(matrix, scale);
	return matrix;
}

static glm::mat4 HRLSkeletal_UFBXMatrixToGLM(const ufbx_matrix& source)
{
	glm::mat4 result(1.f);
	result[0] = glm::vec4((float)source.cols[0].x, (float)source.cols[0].y, (float)source.cols[0].z, 0.f);
	result[1] = glm::vec4((float)source.cols[1].x, (float)source.cols[1].y, (float)source.cols[1].z, 0.f);
	result[2] = glm::vec4((float)source.cols[2].x, (float)source.cols[2].y, (float)source.cols[2].z, 0.f);
	result[3] = glm::vec4((float)source.cols[3].x, (float)source.cols[3].y, (float)source.cols[3].z, 1.f);
	return result;
}

static glm::mat4 HRLSkeletal_ArrayToGLM(const float source[16])
{
	glm::mat4 result(1.f);
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			result[col][row] = source[col * 4 + row];
	return result;
}

static glm::mat4 HRLSkeletal_LerpMatrix(const glm::mat4& a, const glm::mat4& b, float alpha)
{
	const float invAlpha = 1.0f - alpha;
	glm::mat4 result(0.f);
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			result[col][row] = a[col][row] * invAlpha + b[col][row] * alpha;
	return result;
}

static void HRLSkeletal_SetDefaultTransform(HRL_SkeletalBoneTransform& transform)
{
	transform.translation[0] = 0.f;
	transform.translation[1] = 0.f;
	transform.translation[2] = 0.f;
	transform.rotation[0] = 0.f;
	transform.rotation[1] = 0.f;
	transform.rotation[2] = 0.f;
	transform.rotation[3] = 1.f;
	transform.scale[0] = 1.f;
	transform.scale[1] = 1.f;
	transform.scale[2] = 1.f;
}

static HRL_SkeletalBoneTransform HRLSkeletal_LerpTransform(
	const HRL_SkeletalBoneTransform& a,
	const HRL_SkeletalBoneTransform& b,
	float alpha)
{
	HRL_SkeletalBoneTransform result{};
	const glm::vec3 ta(a.translation[0], a.translation[1], a.translation[2]);
	const glm::vec3 tb(b.translation[0], b.translation[1], b.translation[2]);
	const glm::vec3 sa(a.scale[0], a.scale[1], a.scale[2]);
	const glm::vec3 sb(b.scale[0], b.scale[1], b.scale[2]);
	const glm::quat qa(a.rotation[3], a.rotation[0], a.rotation[1], a.rotation[2]);
	const glm::quat qb(b.rotation[3], b.rotation[0], b.rotation[1], b.rotation[2]);
	const glm::vec3 t = glm::mix(ta, tb, alpha);
	const glm::vec3 s = glm::mix(sa, sb, alpha);
	const glm::quat q = glm::normalize(glm::slerp(qa, qb, alpha));

	result.translation[0] = t.x;
	result.translation[1] = t.y;
	result.translation[2] = t.z;
	result.rotation[0] = q.x;
	result.rotation[1] = q.y;
	result.rotation[2] = q.z;
	result.rotation[3] = q.w;
	result.scale[0] = s.x;
	result.scale[1] = s.y;
	result.scale[2] = s.z;
	return result;
}

static void HRLSkeletal_BuildPoseFramesForAnimation(
    HRL_SkeletalMesh* mesh,
    HRL_SkeletalAnimationInternal& animation)
{
    if (!mesh || mesh->bones_.empty() || animation.public_.frameCount == 0 || animation.frames_.empty())
    {
        animation.pose_frames_.clear();
        return;
    }

    const size_t boneCount = mesh->bones_.size();
    const size_t frameCount = animation.public_.frameCount;
    animation.pose_frames_.assign(frameCount * boneCount, glm::mat4(1.f));

    if (animation.pose_matrices_storage_.size() == frameCount * boneCount * 16u)
    {
        for (size_t frame = 0; frame < frameCount; ++frame)
        {
            for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
            {
                const float* src = animation.pose_matrices_storage_.data() + (frame * boneCount + boneIndex) * 16u;
                animation.pose_frames_[frame * boneCount + boneIndex] = HRLSkeletal_ArrayToGLM(src);
            }
        }
        return;
    }

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        std::vector<glm::mat4> world(boneCount, glm::mat4(1.f));
        std::vector<unsigned char> built(boneCount, 0);

        std::function<glm::mat4(size_t)> buildBone = [&](size_t boneIndex) -> glm::mat4
        {
            if (built[boneIndex])
                return world[boneIndex];

            const HRL_SkeletalBoneInternal& bone = mesh->bones_[boneIndex];
            const size_t offset = frame * boneCount + boneIndex;
            glm::mat4 local = HRLSkeletal_TransformToMatrix(animation.frames_[offset]);
            glm::mat4 parentWorld(1.f);
            if (bone.public_.parentIndex < boneCount && bone.public_.parentIndex != boneIndex)
                parentWorld = buildBone((size_t)bone.public_.parentIndex);

            world[boneIndex] = parentWorld * local;
            built[boneIndex] = 1;
            return world[boneIndex];
        };

        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
        {
            const glm::mat4 boneWorld = buildBone(boneIndex);
            animation.pose_frames_[frame * boneCount + boneIndex] =
                boneWorld * mesh->bones_[boneIndex].inverse_bind_;
        }
    }
}

static bool HRLSkeletal_UpdatePose(HRL_SkeletalMesh* mesh)
{
	if (!mesh || mesh->bones_.empty())
		return false;

	mesh->bone_matrices_.resize(mesh->bones_.size(), glm::mat4(1.f));
	const HRL_SkeletalAnimationInternal* animation = nullptr;
	if (mesh->current_animation_ >= 0 && (size_t)mesh->current_animation_ < mesh->animations_.size())
		animation = &mesh->animations_[(size_t)mesh->current_animation_];

	if (animation && !animation->pose_frames_.empty() && animation->public_.frameCount > 0)
	{
		const float duration = std::max(animation->public_.duration, 0.f);
		float time = std::clamp(mesh->animation_time_, 0.f, duration);
		const float framePosition = duration > 0.f
			? time * animation->public_.frameRate
			: 0.f;
		const size_t frame0 = std::min((size_t)std::floor(framePosition), animation->public_.frameCount - 1);
		const size_t frame1 = std::min(frame0 + 1, animation->public_.frameCount - 1);
		const float alpha = frame1 == frame0 ? 0.f : framePosition - (float)frame0;
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
		{
			const size_t offset0 = frame0 * mesh->bones_.size() + boneIndex;
			const size_t offset1 = frame1 * mesh->bones_.size() + boneIndex;
			mesh->bone_matrices_[boneIndex] = HRLSkeletal_LerpMatrix(animation->pose_frames_[offset0], animation->pose_frames_[offset1], alpha);
		}
	}
	else if (!mesh->animations_.empty() && !mesh->animations_.front().pose_frames_.empty())
	{
		// Imported skeletal meshes keep their bind/first-frame skin matrices in
		// world-space form, matching ufbx's geometry_to_world skinning convention.
		const auto& firstPose = mesh->animations_.front().pose_frames_;
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
			mesh->bone_matrices_[boneIndex] = firstPose[boneIndex];
	}
	else
	{
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
			mesh->bone_matrices_[boneIndex] = mesh->bones_[boneIndex].bind_world_ * mesh->bones_[boneIndex].inverse_bind_;
	}

	++mesh->pose_serial_;
	if (mesh->pose_serial_ == 0)
		mesh->pose_serial_ = 1;
	return true;
}

static char* HRLSkeletal_CopyCString(const ufbx_string& string)
{
	char* result = new char[string.length + 1];
	if (string.length > 0)
		std::memcpy(result, string.data, string.length);
	result[string.length] = '\0';
	return result;
}

static void HRLSkeletal_DestroyMeshData(HRL_SkeletalMeshData* data)
{
	if (!data) return;
	if (data->bones)
	{
		for (size_t i = 0; i < data->boneCount; ++i)
			delete[] const_cast<char*>(data->bones[i].name);
		delete[] data->bones;
	}
	if (data->animations)
	{
		for (size_t i = 0; i < data->animationCount; ++i)
		{
			delete[] const_cast<char*>(data->animations[i].name);
			delete[] data->animations[i].frames;
			delete[] data->animations[i].poseMatrices;
		}
		delete[] data->animations;
	}
	delete[] data->vertices;
	delete[] data->indices;
	delete data;
}

static bool HRLSkeletal_FillVertex(
	HRL_SkeletalVertex& output,
	const ufbx_mesh* mesh,
	const ufbx_skin_deformer* skin,
	const std::vector<HRL_uint>& clusterToBone,
	uint32_t meshIndex)
{
	std::memset(&output, 0, sizeof(output));
	const uint32_t vertexIndex = mesh->vertex_indices.data[meshIndex];
	if ((size_t)vertexIndex >= mesh->vertices.count)
		return false;

	ufbx_vec3 position = mesh->vertices.data[vertexIndex];
	if (mesh->vertex_position.exists)
		position = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIndex);
	output.vertex.position[0] = (float)position.x;
	output.vertex.position[1] = (float)position.y;
	output.vertex.position[2] = (float)position.z;

	if (mesh->vertex_normal.exists)
	{
		const ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, meshIndex);
		output.vertex.normal[0] = (float)normal.x;
		output.vertex.normal[1] = (float)normal.y;
		output.vertex.normal[2] = (float)normal.z;
	}
	else
	{
		output.vertex.normal[1] = 1.f;
	}

	if (mesh->vertex_uv.exists)
	{
		const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, meshIndex);
		output.vertex.uv[0] = (float)uv.x;
		output.vertex.uv[1] = (float)uv.y;
	}

	if (mesh->vertex_tangent.exists)
	{
		const ufbx_vec3 tangent = ufbx_get_vertex_vec3(&mesh->vertex_tangent, meshIndex);
		output.vertex.tangent[0] = (float)tangent.x;
		output.vertex.tangent[1] = (float)tangent.y;
		output.vertex.tangent[2] = (float)tangent.z;
	}
	if (mesh->vertex_bitangent.exists)
	{
		const ufbx_vec3 bitangent = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, meshIndex);
		output.vertex.bitangent[0] = (float)bitangent.x;
		output.vertex.bitangent[1] = (float)bitangent.y;
		output.vertex.bitangent[2] = (float)bitangent.z;
	}

	if (!mesh->vertex_tangent.exists || !mesh->vertex_bitangent.exists)
	{
		HRL_Vertex3D tri[3]{};
		// Tangent fallback is completed at the triangle level by the caller. The
		// zeroed tangent space here makes the absence explicit without inventing UVs.
		(void)tri;
	}

	if (skin && (size_t)vertexIndex < skin->vertices.count)
	{
		const ufbx_skin_vertex& skinVertex = skin->vertices.data[vertexIndex];
		const size_t weightEnd = std::min(
			(size_t)skinVertex.weight_begin + (size_t)skinVertex.num_weights,
			skin->weights.count);

		struct Influence {
			HRL_uint bone;
			float weight;
		};
		std::vector<Influence> influences;
		influences.reserve((size_t)skinVertex.num_weights);
		for (size_t weightIndex = skinVertex.weight_begin; weightIndex < weightEnd; ++weightIndex)
		{
			const ufbx_skin_weight& weight = skin->weights.data[weightIndex];
			if ((size_t)weight.cluster_index >= clusterToBone.size())
				continue;
			const HRL_uint boneIndex = clusterToBone[weight.cluster_index];
			if (boneIndex == HRL_INVALID_ID)
				continue;
			const float w = (float)weight.weight;
			if (!std::isfinite(w) || w <= 0.f)
				continue;
			influences.push_back({boneIndex, w});
		}

		std::sort(influences.begin(), influences.end(), [](const Influence& a, const Influence& b) {
			return a.weight > b.weight;
		});

		const size_t influenceCount = std::min(influences.size(), (size_t)HRL_SKELETAL_MAX_INFLUENCES);
		float weightSum = 0.f;
		for (size_t i = 0; i < influenceCount; ++i)
		{
			output.boneIndices[i] = influences[i].bone;
			output.boneWeights[i] = influences[i].weight;
			weightSum += influences[i].weight;
		}
		if (weightSum > 0.f)
		{
			for (size_t i = 0; i < influenceCount; ++i)
				output.boneWeights[i] /= weightSum;
		}
		else if (skin->clusters.count > 0 && !clusterToBone.empty())
		{
			output.boneIndices[0] = clusterToBone[0] == HRL_INVALID_ID ? 0u : clusterToBone[0];
			output.boneWeights[0] = 1.f;
		}
	}
	return true;
}

static void HRLSkeletal_ComputeMissingTangentSpace(
	HRL_SkeletalVertex& a, HRL_SkeletalVertex& b, HRL_SkeletalVertex& c)
{
	const glm::vec3 p0(a.vertex.position[0], a.vertex.position[1], a.vertex.position[2]);
	const glm::vec3 p1(b.vertex.position[0], b.vertex.position[1], b.vertex.position[2]);
	const glm::vec3 p2(c.vertex.position[0], c.vertex.position[1], c.vertex.position[2]);
	const glm::vec2 uv0(a.vertex.uv[0], a.vertex.uv[1]);
	const glm::vec2 uv1(b.vertex.uv[0], b.vertex.uv[1]);
	const glm::vec2 uv2(c.vertex.uv[0], c.vertex.uv[1]);
	glm::vec3 normalA(a.vertex.normal[0], a.vertex.normal[1], a.vertex.normal[2]);
	const glm::vec3 faceNormal = HRLFBX_NormalizeOrFallback(glm::cross(p1-p0, p2-p0), glm::vec3(0,1,0));
	for (glm::vec3* n : {&normalA})
		*n = HRLFBX_NormalizeOrFallback(*n, faceNormal);

	glm::vec3 tangent, bitangent;
	HRLFBX_BuildFallbackTangentSpace(p0,p1,p2,uv0,uv1,uv2,normalA,tangent,bitangent);
	for (HRL_SkeletalVertex* v : {&a,&b,&c})
	{
		glm::vec3 n(v->vertex.normal[0], v->vertex.normal[1], v->vertex.normal[2]);
		n = HRLFBX_NormalizeOrFallback(n, faceNormal);
		v->vertex.normal[0]=n.x; v->vertex.normal[1]=n.y; v->vertex.normal[2]=n.z;
		glm::vec3 t(v->vertex.tangent[0], v->vertex.tangent[1], v->vertex.tangent[2]);
		if (glm::dot(t,t) < 1e-8f) t=tangent;
		t = HRLFBX_NormalizeOrFallback(t - n*glm::dot(n,t), tangent);
		glm::vec3 bt(v->vertex.bitangent[0], v->vertex.bitangent[1], v->vertex.bitangent[2]);
		const glm::vec3 expected = glm::normalize(glm::cross(n,t));
		if (glm::dot(bt,bt) < 1e-8f) bt=expected;
		bt = HRLFBX_NormalizeOrFallback(bt, expected);
		if (glm::dot(bt, expected) < 0.f) bt=-expected;
		v->vertex.tangent[0]=t.x; v->vertex.tangent[1]=t.y; v->vertex.tangent[2]=t.z;
		v->vertex.bitangent[0]=bt.x; v->vertex.bitangent[1]=bt.y; v->vertex.bitangent[2]=bt.z;
	}
}


HRL_SkeletalMeshData* HRL_GetSkeletalMeshFromFBX(const char* _data, size_t _bufferSize)
{
	if (!_data || _bufferSize == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: data and buffer size must be valid");
		return nullptr;
	}

	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.generate_missing_normals = true;
	options.normalize_normals = true;
	options.load_external_files = true;
	options.ignore_missing_external_files = true;
	options.evaluate_skinning = true;
	options.evaluate_caches = false;
	options.ignore_geometry = false;
	options.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
	options.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
	options.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
	options.target_unit_meters = 1.0;

	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_GetSkeletalMeshFromFBX: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	// A FBX file may contain several meshes attached to the same skeleton.
	// This API represents a single HRL skeletal mesh, so pick the largest skinned
	// geometry instead of depending on FBX object ordering (which often puts
	// small accessory meshes before the main character). The diagnostic below
	// makes the limitation explicit instead of silently hiding it.
	const ufbx_mesh* mesh = nullptr;
	size_t skinnedMeshCount = 0;
	size_t largestTriangles = 0;
	for (size_t i = 0; i < scene->meshes.count; ++i)
	{
		const ufbx_mesh* candidate = scene->meshes.data[i];
		if (!candidate || candidate->skin_deformers.count == 0 || candidate->num_triangles == 0)
			continue;
		++skinnedMeshCount;
		if (!mesh || candidate->num_triangles > largestTriangles)
		{
			mesh = candidate;
			largestTriangles = candidate->num_triangles;
		}
	}
	if (skinnedMeshCount > 1)
	{
		std::printf(
			"HRL_GetSkeletalMeshFromFBX: FBX contains %zu skinned meshes; importing the largest (%zu triangles) with the single-mesh API.\n",
			skinnedMeshCount, largestTriangles);
	}
	if (!mesh)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: FBX scene contains no skinned mesh geometry");
		return nullptr;
	}

	const ufbx_node* meshNode = nullptr;
	for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
	{
		const ufbx_node* candidate = scene->nodes.data[nodeIndex];
		if (candidate && candidate->mesh == mesh)
		{
			meshNode = candidate;
			break;
		}
	}
	if (!meshNode && mesh->instances.count > 0)
		meshNode = mesh->instances.data[0];

	const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
	if (!skin || skin->clusters.count == 0)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: mesh has no skin clusters");
		return nullptr;
	}

	try
	{
		std::unordered_map<const ufbx_node*, bool> nodeSet;
		std::unordered_map<const ufbx_node*, const ufbx_skin_cluster*> clusterByNode;
		std::vector<const ufbx_node*> boneNodes;
		for (size_t i = 0; i < skin->clusters.count; ++i)
		{
			const ufbx_skin_cluster* cluster = skin->clusters.data[i];
			if (!cluster || !cluster->bone_node)
				continue;
			clusterByNode.emplace(cluster->bone_node, cluster);
			for (ufbx_node* node = cluster->bone_node; node; node = node->parent)
			{
				if (!nodeSet.emplace(node, true).second)
					break;
				boneNodes.push_back(node);
			}
		}
		if (boneNodes.empty())
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skin has no valid bone nodes");
			return nullptr;
		}
		std::stable_sort(boneNodes.begin(), boneNodes.end(), [](const ufbx_node* a, const ufbx_node* b){
			if (a->node_depth != b->node_depth) return a->node_depth < b->node_depth;
			if (a->name.length != b->name.length) return a->name.length < b->name.length;
			return std::strncmp(a->name.data, b->name.data, a->name.length) < 0;
		});
		if (boneNodes.size() > HRL_MAX_SKELETAL_BONES)
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skeleton exceeds HRL_MAX_SKELETAL_BONES");
			return nullptr;
		}

		std::unordered_map<const ufbx_node*, HRL_uint> nodeToBone;
		for (size_t i = 0; i < boneNodes.size(); ++i)
			nodeToBone[boneNodes[i]] = (HRL_uint)i;

		std::vector<HRL_uint> clusterToBone(skin->clusters.count, HRL_INVALID_ID);
		for (size_t i = 0; i < skin->clusters.count; ++i)
		{
			const ufbx_skin_cluster* cluster = skin->clusters.data[i];
			if (!cluster || !cluster->bone_node) continue;
			auto it = nodeToBone.find(cluster->bone_node);
			if (it != nodeToBone.end()) clusterToBone[i] = it->second;
		}

		// Keep mesh vertices in ufbx's native geometry space. IMPORTANT: the
		// skinning matrices themselves are world-space matrices. This is the same
		// convention used by the official ufbx viewer: for each influence,
		// currentBoneWorld * geometry_to_bone is applied directly to the geometry
		// vertex. Therefore the HRL object/model transform must NOT apply the FBX
		// geometry_to_world a second time.

		std::vector<HRL_SkeletalVertex> vertices;
		std::vector<HRL_uint> indices;
		vertices.reserve(mesh->num_triangles * 3u);
		indices.reserve(mesh->num_triangles * 3u);
		const size_t triangleCapacity = (size_t)std::max<uint32_t>(1u, mesh->max_face_triangles) * 3u;
		std::vector<uint32_t> triangles(triangleCapacity);
		for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
		{
			const ufbx_face& face = mesh->faces.data[faceIndex];
			if (face.num_indices < 3) continue;
			const uint32_t triangleCount = ufbx_triangulate_face(triangles.data(), triangles.size(), mesh, face);
			for (uint32_t tri = 0; tri < triangleCount; ++tri)
			{
				HRL_SkeletalVertex triVertices[3]{};
				bool valid = true;
				for (size_t corner = 0; corner < 3; ++corner)
				{
					const uint32_t meshIndex = triangles[(size_t)tri * 3u + corner];
					if ((size_t)meshIndex >= mesh->num_indices ||
						!HRLSkeletal_FillVertex(triVertices[corner], mesh, skin, clusterToBone, meshIndex))
					{
						valid = false;
						break;
					}
				}
				if (!valid) continue;
				HRLSkeletal_ComputeMissingTangentSpace(triVertices[0], triVertices[1], triVertices[2]);
				for (int corner = 0; corner < 3; ++corner)
				{
					const HRL_uint index = (HRL_uint)vertices.size();
					vertices.push_back(triVertices[corner]);
					indices.push_back(index);
				}
			}
		}

		if (vertices.empty())
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skinned mesh contains no convertible triangles");
			return nullptr;
		}

		HRL_SkeletalMeshData* result = new HRL_SkeletalMeshData{};
		// Skeletal vertices are deformed all the way to world space by the skin
		// matrices. Keep the generic geometry transform field neutral so the
		// renderer cannot apply the FBX node transform a second time.
		for (int i = 0; i < 16; ++i)
			result->geometryToWorldMatrix[i] = 0.f;
		result->geometryToWorldMatrix[0] = 1.f;
		result->geometryToWorldMatrix[5] = 1.f;
		result->geometryToWorldMatrix[10] = 1.f;
		result->geometryToWorldMatrix[15] = 1.f;
		result->vertexCount = vertices.size();
		result->indexCount = indices.size();
		result->boneCount = boneNodes.size();
		result->animationCount = scene->anim_stacks.count > 0 ? scene->anim_stacks.count : 1;
		result->vertices = new HRL_SkeletalVertex[result->vertexCount];
		result->indices = new HRL_uint[result->indexCount];
		result->bones = new HRL_SkeletalBone[result->boneCount]{};
		result->animations = new HRL_SkeletalAnimation[result->animationCount]{};
		std::copy(vertices.begin(), vertices.end(), result->vertices);
		std::copy(indices.begin(), indices.end(), result->indices);

		for (size_t i = 0; i < boneNodes.size(); ++i)
		{
			const ufbx_node* node = boneNodes[i];
			HRL_SkeletalBone& bone = result->bones[i];
			bone.name = HRLSkeletal_CopyCString(node->name);
			bone.parentIndex = HRL_INVALID_ID;
			for (ufbx_node* parent = node->parent; parent; parent = parent->parent)
			{
				auto pit = nodeToBone.find(parent);
				if (pit != nodeToBone.end()) { bone.parentIndex = pit->second; break; }
			}
			const auto clusterIt = clusterByNode.find(node);
			const glm::mat4 inverseBind = clusterIt != clusterByNode.end()
				? HRLSkeletal_UFBXMatrixToGLM(clusterIt->second->geometry_to_bone)
				: glm::mat4(1.f);
			const HRL_SkeletalBoneTransform bindTransform = HRLSkeletal_TransformFromUFBX(node->local_transform);
			bone.bindTransform = bindTransform;
			const glm::mat4 bindWorld = HRLSkeletal_UFBXMatrixToGLM(node->node_to_world);
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
				{
					bone.bindWorldMatrix[c * 4 + r] = bindWorld[c][r];
					bone.inverseBindMatrix[c * 4 + r] = inverseBind[c][r];
				}
		}

		for (size_t animationIndex = 0; animationIndex < result->animationCount; ++animationIndex)
		{
			HRL_SkeletalAnimation& animation = result->animations[animationIndex];
			const bool bindPose = scene->anim_stacks.count == 0;
			const ufbx_anim_stack* stack = bindPose ? nullptr : scene->anim_stacks.data[animationIndex];
			if (bindPose)
			{
				const char* name = "BindPose";
				animation.name = new char[std::strlen(name) + 1];
				std::strcpy(const_cast<char*>(animation.name), name);
				animation.duration = 0.f;
				animation.frameRate = 30.f;
				animation.frameCount = 1;
			}
			else
			{
				animation.name = HRLSkeletal_CopyCString(stack->name);
				animation.duration = (float)std::max(0.0, stack->time_end - stack->time_begin);
				animation.frameRate = 30.f;
				const size_t maxFrames = 4096;
				animation.frameCount = animation.duration > 0.f
					? std::min(maxFrames, (size_t)std::ceil(animation.duration * animation.frameRate) + 1u)
					: 1u;
			}
			if (result->boneCount != 0 && animation.frameCount > std::numeric_limits<size_t>::max() / result->boneCount)
			{
				ufbx_free_scene(scene);
				HRLSkeletal_DestroyMeshData(result);
				SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: animation frame data is too large");
				return nullptr;
			}
			const size_t frameValueCount = animation.frameCount * result->boneCount;
			if (frameValueCount > std::numeric_limits<size_t>::max() / 16u)
			{
				ufbx_free_scene(scene);
				HRLSkeletal_DestroyMeshData(result);
				SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated pose data is too large");
				return nullptr;
			}
			animation.frames = new HRL_SkeletalBoneTransform[frameValueCount]{};
			animation.poseMatrices = new float[frameValueCount * 16u]{};
			for (size_t frame = 0; frame < animation.frameCount; ++frame)
			{
				const double time = bindPose || !stack
					? 0.0
					: std::min(stack->time_end, stack->time_begin + (double)frame / (double)animation.frameRate);
				ufbx_scene* evaluatedScene = nullptr;
				if (!bindPose && stack && stack->anim)
				{
					ufbx_error evalError{};
					evaluatedScene = ufbx_evaluate_scene(scene, stack->anim, time, nullptr, &evalError);
					if (!evaluatedScene)
					{
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: failed to evaluate animation scene");
						return nullptr;
					}
				}

				// ufbx directly provides the current skinning transform as
				// cluster->geometry_to_world = bone_node->node_to_world * geometry_to_bone.
				// Use that instead of rebuilding the FBX bone hierarchy manually.
				std::unordered_map<uint32_t, const ufbx_skin_cluster*> evaluatedClusterByNode;
				if (evaluatedScene)
				{
					const uint32_t meshTypedId = mesh->typed_id;
					if ((size_t)meshTypedId >= evaluatedScene->meshes.count || !evaluatedScene->meshes.data[meshTypedId])
					{
						ufbx_free_scene(evaluatedScene);
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated mesh lookup failed");
						return nullptr;
					}
					const ufbx_mesh* evaluatedMesh = evaluatedScene->meshes.data[meshTypedId];
					if (!evaluatedMesh || evaluatedMesh->skin_deformers.count == 0 || !evaluatedMesh->skin_deformers.data[0])
					{
						ufbx_free_scene(evaluatedScene);
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated mesh has no skin deformer");
						return nullptr;
					}
					const ufbx_skin_deformer* evaluatedSkin = evaluatedMesh->skin_deformers.data[0];
					for (size_t clusterIndex = 0; clusterIndex < evaluatedSkin->clusters.count; ++clusterIndex)
					{
						const ufbx_skin_cluster* cluster = evaluatedSkin->clusters.data[clusterIndex];
						if (cluster && cluster->bone_node)
							evaluatedClusterByNode[cluster->bone_node->typed_id] = cluster;
					}
				}


				for (size_t boneIndex = 0; boneIndex < boneNodes.size(); ++boneIndex)
				{
					const size_t offset = frame * boneNodes.size() + boneIndex;
					const ufbx_skin_cluster* cluster = nullptr;
					const auto clusterIt = clusterByNode.find(boneNodes[boneIndex]);
					if (clusterIt != clusterByNode.end()) cluster = clusterIt->second;
					const ufbx_skin_cluster* poseCluster = cluster;
					if (evaluatedScene)
					{
						auto evaluatedIt = evaluatedClusterByNode.find(boneNodes[boneIndex]->typed_id);
						if (evaluatedIt != evaluatedClusterByNode.end()) poseCluster = evaluatedIt->second;
					}

					HRL_SkeletalBoneTransform transform = HRLSkeletal_TransformFromUFBX(boneNodes[boneIndex]->local_transform);
					if (evaluatedScene)
					{
						const uint32_t typedId = boneNodes[boneIndex]->typed_id;
						if ((size_t)typedId < evaluatedScene->nodes.count && evaluatedScene->nodes.data[typedId])
							transform = HRLSkeletal_TransformFromUFBX(evaluatedScene->nodes.data[typedId]->local_transform);
						else
						{
							ufbx_free_scene(evaluatedScene);
							ufbx_free_scene(scene);
							HRLSkeletal_DestroyMeshData(result);
							SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated bone node lookup failed");
							return nullptr;
						}
					}
					animation.frames[offset] = transform;
					// ufbx's cluster->geometry_to_world is already the complete current
					// skin matrix:
					//     bone_node->node_to_world * geometry_to_bone
					// Keep it in world space exactly as provided by ufbx. Do not pre/post-
					// multiply it by the mesh node transform: doing so is the source of
					// the disappearing/partial-mesh regression between the previous fixes.
					const glm::mat4 poseMatrix = poseCluster
						? HRLSkeletal_UFBXMatrixToGLM(poseCluster->geometry_to_world)
						: glm::mat4(1.f);
					for (int col = 0; col < 4; ++col)
						for (int row = 0; row < 4; ++row)
							animation.poseMatrices[offset * 16u + (size_t)col * 4u + (size_t)row] = poseMatrix[col][row];
				}
				if (evaluatedScene)
					ufbx_free_scene(evaluatedScene);
			}
		}

		ufbx_free_scene(scene);
		return result;
	}
	catch (const std::bad_alloc&)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: out of memory while converting skeleton");
		return nullptr;
	}
}

void HRL_FreeSkeletalMeshData(HRL_SkeletalMeshData* _data)
{
	HRLSkeletal_DestroyMeshData(_data);
}


HRL_id HRL_CreateSkeletalMesh(HRL_id _sceneid, const HRL_SkeletalMeshData* _data)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (!_data || !_data->vertices || _data->vertexCount < 3 || !_data->bones || _data->boneCount == 0 || _data->boneCount > HRL_MAX_SKELETAL_BONES)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: invalid skeletal mesh data");
		return HRL_INVALID_ID;
	}
	if (_data->indexCount > 0 && !_data->indices)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index count is non-zero but indices are null");
		return HRL_INVALID_ID;
	}
	if (_data->vertexCount > (size_t)std::numeric_limits<HRL_uint>::max())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex count exceeds HRL_uint index range");
		return HRL_INVALID_ID;
	}
	for (size_t vertexIndex = 0; vertexIndex < _data->vertexCount; ++vertexIndex)
	{
		const HRL_Vertex3D& vertex = _data->vertices[vertexIndex].vertex;
		for (float value : vertex.position)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex position contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.normal)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex normal contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.uv)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex UV contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.tangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex tangent contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.bitangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex bitangent contains NaN or infinity"); return HRL_INVALID_ID; }
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = _data->vertices[vertexIndex].boneWeights[influence];
			if (!std::isfinite(weight) || weight < 0.f)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: bone weights must be finite and non-negative");
				return HRL_INVALID_ID;
			}
			if (weight > 0.f && _data->vertices[vertexIndex].boneIndices[influence] >= _data->boneCount)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: bone index is outside the skeleton");
				return HRL_INVALID_ID;
			}
			weightSum += weight;
		}
		if (!(weightSum > 0.f) || !std::isfinite(weightSum))
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: every vertex must have at least one positive bone weight");
			return HRL_INVALID_ID;
		}
	}
	for (size_t i = 0; i < _data->indexCount; ++i)
	{
		if ((size_t)_data->indices[i] >= _data->vertexCount)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index references a vertex outside the vertex array");
			return HRL_INVALID_ID;
		}
	}
	if (_data->indexCount > 0 && (_data->indexCount % 3u) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index count must be a multiple of 3");
		return HRL_INVALID_ID;
	}

	std::vector<HRL_uint> generatedIndices;
	const HRL_uint* indices = _data->indices;
	if (_data->indexCount == 0)
	{
		if ((_data->vertexCount % 3u) != 0)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: non-indexed vertex count must be a multiple of 3");
			return HRL_INVALID_ID;
		}
		generatedIndices.resize(_data->vertexCount);
		for (size_t i = 0; i < generatedIndices.size(); ++i)
			generatedIndices[i] = (HRL_uint)i;
		indices = generatedIndices.data();
	}

	HRL_id newId = GenerateHRL_ID();
	if (!g_Backend.RHI_CreateSkeletalMesh ||
		g_Backend.RHI_CreateSkeletalMesh(newId, _data->vertices, _data->vertexCount, indices,
			_data->indexCount > 0 ? _data->indexCount : generatedIndices.size(), (HRL_uint)_data->boneCount) != HRL_TRUE)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: backend failed to create skeletal mesh");
		return HRL_INVALID_ID;
	}

	std::unique_ptr<HRL_SkeletalMesh> mesh(new (std::nothrow) HRL_SkeletalMesh());
	if (!mesh)
	{
		g_Backend.RHI_DeleteMesh(newId);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: out of memory");
		return HRL_INVALID_ID;
	}
	mesh->scene_ = _sceneid;
	mesh->type_ = HRL_3D_SKELETAL_MESH;
	bool hasGeometryToWorld = false;
	for (int i = 0; i < 16; ++i)
		if (std::isfinite(_data->geometryToWorldMatrix[i]) && std::fabs(_data->geometryToWorldMatrix[i]) > 0.f) { hasGeometryToWorld = true; break; }
	mesh->fbx_geometry_to_world_ = hasGeometryToWorld
		? HRLSkeletal_ArrayToGLM(_data->geometryToWorldMatrix)
		: glm::mat4(1.f);
	mesh->triangle_count_ = (_data->indexCount > 0 ? _data->indexCount : generatedIndices.size()) / 3u;
	mesh->vertices_.assign(_data->vertices, _data->vertices + _data->vertexCount);
	mesh->indices_.assign(indices, indices + (_data->indexCount > 0 ? _data->indexCount : generatedIndices.size()));
	mesh->bones_.resize(_data->boneCount);
	for (size_t i = 0; i < _data->boneCount; ++i)
	{
		mesh->bones_[i].public_ = _data->bones[i];
		mesh->bones_[i].name_storage_ = _data->bones[i].name ? _data->bones[i].name : "Bone";
		mesh->bones_[i].public_.parentIndex = _data->bones[i].parentIndex < _data->boneCount ? _data->bones[i].parentIndex : HRL_INVALID_ID;
		for (int j = 0; j < 16; ++j)
			mesh->bones_[i].public_.inverseBindMatrix[j] = _data->bones[i].inverseBindMatrix[j];
		mesh->bones_[i].bind_local_ = _data->bones[i].bindTransform;
		glm::mat4 inverseBind(1.0f);
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				inverseBind[col][row] = mesh->bones_[i].public_.inverseBindMatrix[col * 4 + row];
		mesh->bones_[i].inverse_bind_ = inverseBind;
		mesh->bones_[i].bind_world_ = HRLSkeletal_ArrayToGLM(_data->bones[i].bindWorldMatrix);
	}
	// Bind world matrices from the importer are exact; reconstruct only for legacy manually-created data.
	{
		const size_t boneCount = mesh->bones_.size();
		std::vector<unsigned char> built(boneCount, 0);
		std::function<glm::mat4(size_t)> buildBone = [&](size_t boneIndex) -> glm::mat4
		{
			if (built[boneIndex])
				return mesh->bones_[boneIndex].bind_world_;
			glm::mat4 parentWorld(1.f);
			const size_t parent = mesh->bones_[boneIndex].public_.parentIndex;
			if (parent < boneCount && parent != boneIndex)
				parentWorld = buildBone(parent);
			mesh->bones_[boneIndex].bind_world_ = parentWorld * HRLSkeletal_TransformToMatrix(mesh->bones_[boneIndex].bind_local_);
			built[boneIndex] = 1;
			return mesh->bones_[boneIndex].bind_world_;
		};
		for (size_t i = 0; i < boneCount; ++i)
		{
			bool hasBindWorld = false;
			for (int e = 0; e < 16; ++e)
				if (mesh->bones_[i].public_.bindWorldMatrix[e] != 0.f) { hasBindWorld = true; break; }
			if (!hasBindWorld)
				buildBone(i);
		}
	}

	// The baked animations are copied before public pointers are refreshed.
	mesh->animations_.resize(_data->animationCount);
	for (size_t i = 0; i < _data->animationCount; ++i)
	{
		mesh->animations_[i].public_ = _data->animations[i];
		mesh->animations_[i].name_storage_ = _data->animations[i].name ? _data->animations[i].name : "Animation";
		if (_data->boneCount != 0 && _data->animations[i].frameCount > std::numeric_limits<size_t>::max() / _data->boneCount)
		{
			g_Backend.RHI_DeleteMesh(newId);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: animation frame data is too large");
			return HRL_INVALID_ID;
		}
		const size_t valueCount = _data->animations[i].frameCount * _data->boneCount;
		if (valueCount > 0 && !_data->animations[i].frames)
		{
			g_Backend.RHI_DeleteMesh(newId);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: animation has no frame data");
			return HRL_INVALID_ID;
		}
		if (valueCount > 0)
			mesh->animations_[i].frames_.assign(_data->animations[i].frames, _data->animations[i].frames + valueCount);
		else
			mesh->animations_[i].frames_.clear();
		if (valueCount > 0 && _data->animations[i].poseMatrices)
			mesh->animations_[i].pose_matrices_storage_.assign(_data->animations[i].poseMatrices, _data->animations[i].poseMatrices + valueCount * 16u);
		else
			mesh->animations_[i].pose_matrices_storage_.clear();
	}

	for (auto& animation : mesh->animations_)
		HRLSkeletal_BuildPoseFramesForAnimation(mesh.get(), animation);

	mesh->RefreshPublicPointers();
	mesh->bone_matrices_.resize(mesh->bones_.size(), glm::mat4(1.f));
	HRLSkeletal_UpdatePose(mesh.get());

	glm::vec3 minPoint(std::numeric_limits<float>::max());
	glm::vec3 maxPoint(std::numeric_limits<float>::lowest());
	for (const auto& vertex : mesh->vertices_)
	{
		const glm::vec4 localPosition(vertex.vertex.position[0], vertex.vertex.position[1], vertex.vertex.position[2], 1.f);
		glm::vec4 skinnedPosition(0.f);
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = vertex.boneWeights[influence];
			if (!(weight > 0.f)) continue;
			const size_t boneIndex = (size_t)vertex.boneIndices[influence];
			if (boneIndex >= mesh->bone_matrices_.size()) continue;
			skinnedPosition += mesh->bone_matrices_[boneIndex] * localPosition * weight;
			weightSum += weight;
		}
		if (!(weightSum > 0.f))
			skinnedPosition = localPosition;
		else if (std::abs(weightSum - 1.f) > 1e-5f)
			skinnedPosition /= weightSum;
		const glm::vec3 p = glm::vec3(skinnedPosition);
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
		{
			minPoint = glm::min(minPoint, p);
			maxPoint = glm::max(maxPoint, p);
		}
	}
	if (!std::isfinite(minPoint.x) || !std::isfinite(maxPoint.x))
	{
		minPoint = glm::vec3(-1.f);
		maxPoint = glm::vec3(1.f);
	}
	mesh->bounds_center_ = (minPoint + maxPoint) * 0.5f;
	float radius2 = 0.f;
	for (const auto& vertex : mesh->vertices_)
	{
		const glm::vec4 localPosition(vertex.vertex.position[0], vertex.vertex.position[1], vertex.vertex.position[2], 1.f);
		glm::vec4 skinnedPosition(0.f);
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = vertex.boneWeights[influence];
			if (!(weight > 0.f)) continue;
			const size_t boneIndex = (size_t)vertex.boneIndices[influence];
			if (boneIndex >= mesh->bone_matrices_.size()) continue;
			skinnedPosition += mesh->bone_matrices_[boneIndex] * localPosition * weight;
			weightSum += weight;
		}
		if (!(weightSum > 0.f)) skinnedPosition = localPosition;
		else if (std::abs(weightSum - 1.f) > 1e-5f) skinnedPosition /= weightSum;
		const glm::vec3 p = glm::vec3(skinnedPosition);
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
			radius2 = std::max(radius2, glm::dot(p - mesh->bounds_center_, p - mesh->bounds_center_));
	}
	mesh->bounds_radius_ = std::sqrt(radius2);

	HRL_SkeletalMesh* rawMesh = mesh.release();
	sceneIt->second->meshes.emplace(newId, rawMesh);
	++sceneIt->second->mesh_revision;
	sceneIt->second->shadows_dirty = true;
	ctx_.meshes.emplace(newId, rawMesh);
	return newId;
}

HRL_uint HRL_GetSkeletalBoneCount(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBoneCount: invalid skeletal mesh ID");
		return 0;
	}
	return (HRL_uint)static_cast<HRL_SkeletalMesh*>(it->second)->bones_.size();
}

const HRL_SkeletalBone* HRL_GetSkeletalBone(HRL_id _meshid, HRL_uint _index)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBone: invalid skeletal mesh ID");
		return nullptr;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_index >= mesh->bones_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBone: invalid bone index");
		return nullptr;
	}
	return &mesh->bones_[_index].public_;
}

HRL_uint HRL_FindSkeletalBone(HRL_id _meshid, const char* _name)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindSkeletalBone: invalid mesh or bone name");
		return HRL_INVALID_ID;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	for (size_t i = 0; i < mesh->bones_.size(); ++i)
		if (mesh->bones_[i].name_storage_ == _name)
			return (HRL_uint)i;
	return HRL_INVALID_ID;
}

HRL_uint HRL_GetSkeletalAnimationCount(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimationCount: invalid skeletal mesh ID");
		return 0;
	}
	return (HRL_uint)static_cast<HRL_SkeletalMesh*>(it->second)->animations_.size();
}

const HRL_SkeletalAnimation* HRL_GetSkeletalAnimation(HRL_id _meshid, HRL_uint _index)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimation: invalid skeletal mesh ID");
		return nullptr;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_index >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimation: invalid animation index");
		return nullptr;
	}
	return &mesh->animations_[_index].public_;
}

HRL_uint HRL_FindSkeletalAnimation(HRL_id _meshid, const char* _name)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindSkeletalAnimation: invalid mesh or animation name");
		return HRL_INVALID_ID;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	for (size_t i = 0; i < mesh->animations_.size(); ++i)
		if (mesh->animations_[i].name_storage_ == _name)
			return (HRL_uint)i;
	return HRL_INVALID_ID;
}

void HRL_PlaySkeletalAnimation(HRL_id _meshid, HRL_uint _animation)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_PlaySkeletalAnimation: invalid skeletal mesh ID");
		return;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_animation >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_PlaySkeletalAnimation: invalid animation index");
		return;
	}
	mesh->current_animation_ = (int)_animation;
	mesh->animation_time_ = 0.f;
	mesh->animation_playing_ = true;
	HRLSkeletal_UpdatePose(mesh);
}

void HRL_StopSkeletalAnimation(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_StopSkeletalAnimation: invalid skeletal mesh ID");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_playing_ = false;
}

void HRL_SetSkeletalAnimationTime(HRL_id _meshid, float _time)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: invalid skeletal mesh ID");
		return;
	}
	if (!std::isfinite(_time))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: time must be finite");
		return;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (mesh->current_animation_ < 0 || (size_t)mesh->current_animation_ >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: no animation is selected");
		return;
	}
	const float duration = std::max(mesh->animations_[(size_t)mesh->current_animation_].public_.duration, 0.f);
	if (mesh->animation_loop_ && duration > 0.f)
	{
		float wrapped = std::fmod(_time, duration);
		if (wrapped < 0.f) wrapped += duration;
		mesh->animation_time_ = wrapped;
	}
	else
		mesh->animation_time_ = std::clamp(_time, 0.f, duration);
	HRLSkeletal_UpdatePose(mesh);
}

void HRL_SetSkeletalAnimationSpeed(HRL_id _meshid, float _speed)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationSpeed: invalid skeletal mesh ID");
		return;
	}
	if (!std::isfinite(_speed))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationSpeed: speed must be finite");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_speed_ = _speed;
}

void HRL_SetSkeletalAnimationLoop(HRL_id _meshid, int _loop)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationLoop: invalid skeletal mesh ID");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_loop_ = _loop != HRL_FALSE;
}

int HRL_GetCurrentSkeletalAnimation(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetCurrentSkeletalAnimation: invalid skeletal mesh ID");
		return -1;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->current_animation_;
}

float HRL_GetSkeletalAnimationTime(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimationTime: invalid skeletal mesh ID");
		return 0.f;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->animation_time_;
}

int HRL_IsSkeletalAnimationPlaying(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_IsSkeletalAnimationPlaying: invalid skeletal mesh ID");
		return HRL_FALSE;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->animation_playing_ ? HRL_TRUE : HRL_FALSE;
}

void HRL_UpdateSkeletalAnimations(float _deltaSeconds)
{
	if (!std::isfinite(_deltaSeconds) || _deltaSeconds < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_UpdateSkeletalAnimations: delta seconds must be finite and non-negative");
		return;
	}
	for (const auto& [id, baseMesh] : ctx_.meshes)
	{
		(void)id;
		if (!baseMesh || baseMesh->type_ != HRL_3D_SKELETAL_MESH)
			continue;
		auto* mesh = static_cast<HRL_SkeletalMesh*>(baseMesh);
		if (!mesh->animation_playing_ || mesh->current_animation_ < 0 || (size_t)mesh->current_animation_ >= mesh->animations_.size())
			continue;
		const float duration = std::max(mesh->animations_[(size_t)mesh->current_animation_].public_.duration, 0.f);
		if (duration <= 0.f)
		{
			mesh->animation_time_ = 0.f;
			mesh->animation_playing_ = false;
			HRLSkeletal_UpdatePose(mesh);
			continue;
		}
		mesh->animation_time_ += _deltaSeconds * mesh->animation_speed_;
		if (mesh->animation_loop_)
		{
			mesh->animation_time_ = std::fmod(mesh->animation_time_, duration);
			if (mesh->animation_time_ < 0.f) mesh->animation_time_ += duration;
		}
		else if (mesh->animation_time_ >= duration)
		{
			mesh->animation_time_ = duration;
			mesh->animation_playing_ = false;
		}
		else if (mesh->animation_time_ <= 0.f)
		{
			mesh->animation_time_ = 0.f;
			if (mesh->animation_speed_ < 0.f)
				mesh->animation_playing_ = false;
		}
		const bool poseChanged = HRLSkeletal_UpdatePose(mesh);
		if (poseChanged)
		{
			auto sceneIt = ctx_.scenes.find(mesh->scene_);
			if (sceneIt != ctx_.scenes.end() && sceneIt->second)
				sceneIt->second->shadows_dirty = true;
		}
	}
}
