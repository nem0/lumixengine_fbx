#include "animation/animation.h"
#include "editor/platform_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/fs/os_file.h"
#include "engine/iplugin.h"
#include "engine/lua_wrapper.h"
#include "engine/path_utils.h"
#include "imgui/imgui.h"
#include "renderer/model.h"
#include <fbxsdk.h>


namespace Lumix
{ 


LUMIX_PLUGIN_ENTRY(lumixengine_fbx)
{
	return nullptr;
}


struct ImportFBXPlugin LUMIX_FINAL : public StudioApp::IPlugin
{
	struct ImportMaterial
	{
		FbxSurfaceMaterial* fbx = nullptr;
		bool import = true;
		bool alpha_cutout = false;
	};

	struct ImportMesh
	{
		FbxMesh* fbx = nullptr;
		bool import = true;
		bool import_physics = false;
		int lod = 0;
	};

	#pragma pack(1)
	struct Vertex
	{
		float px, py, pz;
		u32 normal;
		float u, v;
		i16 indices[4];
		float weights[4];
	};
	#pragma pack()


	static u32 packuint32(u8 _x, u8 _y, u8 _z, u8 _w)
	{
		union {
			u32 ui32;
			u8 arr[4];
		} un;

		un.arr[0] = _x;
		un.arr[1] = _y;
		un.arr[2] = _z;
		un.arr[3] = _w;

		return un.ui32;
	}


	static u32 packF4u(const FbxVector4& vec)
	{
		const u8 xx = u8(vec.mData[0] * 127.0f + 128.0f);
		const u8 yy = u8(vec.mData[1] * 127.0f + 128.0f);
		const u8 zz = u8(vec.mData[2] * 127.0f + 128.0f);
		const u8 ww = u8(0);
		return packuint32(xx, yy, zz, ww);
	}

	template <typename T> static int indexOf(const T* const* array, const T* obj)
	{
		int i = 0;
		const T* const* iter = array;
		while (*iter != obj)
		{
			++i;
			++iter;
		}
		return i;
	}


	FbxMesh* getAnyMeshFromBone(FbxNode* node) const
	{
		for (int i = 0; i < meshes.size(); ++i)
		{
			FbxMesh* mesh = meshes[i].fbx;
			FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
			auto* skin = static_cast<FbxSkin*>(deformer);
			int cluster_count = skin->GetClusterCount();
			for (int j = 0; j < cluster_count; ++j)
			{
				FbxCluster* cluster = skin->GetCluster(j);
				if (cluster->GetLink() == node) return mesh;
			}
		}

		return nullptr;
	}


	static FbxAMatrix getBindPoseMatrix(FbxMesh* mesh, FbxNode* node)
	{
		if (!mesh) return FbxAMatrix();

		const FbxAMatrix geometry_matrix(
			mesh->GetNode()->GetGeometricTranslation(FbxNode::eSourcePivot),
			mesh->GetNode()->GetGeometricRotation(FbxNode::eSourcePivot),
			mesh->GetNode()->GetGeometricScaling(FbxNode::eSourcePivot));

		FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
		auto* skin = static_cast<FbxSkin*>(deformer);

		FbxCluster* cluster = skin->GetCluster(0);
		int cluster_count = skin->GetClusterCount();
		for (int i = 0; i < cluster_count; ++i)
		{
			if (skin->GetCluster(i)->GetLink() == node)
			{
				cluster = skin->GetCluster(i);
				break;
			}
		}

		FbxAMatrix transform_matrix;
		cluster->GetTransformMatrix(transform_matrix);
		transform_matrix *= geometry_matrix;

		FbxAMatrix transform_link_matrix;
		cluster->GetTransformLinkMatrix(transform_link_matrix);

		const FbxAMatrix inverse_bind_pose =
			transform_link_matrix.Inverse() /** transform_matrix*/;

		auto scale = transform_link_matrix.GetS().mData[0];
		// TODO check this in isValid function
		// assert(scale > 0.99 && scale < 1.01);

		const FbxAMatrix bind_pose = inverse_bind_pose.Inverse();

		return bind_pose;
	}


	void gatherMaterials(FbxNode* node)
	{
		for (int i = 0; i < node->GetMaterialCount(); ++i)
		{
			materials.emplace().fbx = node->GetMaterial(i);
		}

		for (int i = 0; i < node->GetChildCount(); ++i)
		{
			gatherMaterials(node->GetChild(i));
		}
	}


	static void gatherBones(FbxNode* node, Array<FbxNode*>& bones)
	{
		bool is_bone = node->GetNodeAttribute() && node->GetNodeAttribute()->GetAttributeType() == FbxNodeAttribute::EType::eSkeleton;
		// TODO use is_bone
		bones.push(node);

		int count = 1;
		for (int i = 0; i < node->GetChildCount(); ++i)
		{
			gatherBones(node->GetChild(i), bones);
		}
	}


	void gatherMeshes(FbxNode* node)
	{
		FbxMesh* mesh = node->GetMesh();
		if (mesh) meshes.emplace().fbx = mesh;

		for (int i = 0; i < node->GetChildCount(); ++i)
		{
			gatherMeshes(node->GetChild(i));
		}
	}


	static bool isValid(FbxMesh** meshes, int mesh_count)
	{
		// TODO call this function
		// TODO error message
		for (int i = 0; i < mesh_count; ++i)
		{
			FbxMesh* mesh = meshes[i];
			if (!mesh->IsTriangleMesh()) return false;
			if (mesh->GetDeformerCount() > 1) return false;
		}
		return true;
	}


	static Vec3 toLumixVec3(const FbxVector4& v) { return{ (float)v.mData[0], (float)v.mData[1], (float)v.mData[2] }; }


	static Matrix toLumix(const FbxAMatrix& mtx)
	{
		Matrix res;

		res.m11 = (float)mtx.mData[0].mData[0];
		res.m12 = (float)mtx.mData[0].mData[1];
		res.m13 = (float)mtx.mData[0].mData[2];
		res.m14 = (float)mtx.mData[0].mData[3];

		res.m21 = (float)mtx.mData[1].mData[0];
		res.m22 = (float)mtx.mData[1].mData[1];
		res.m23 = (float)mtx.mData[1].mData[2];
		res.m24 = (float)mtx.mData[1].mData[3];

		res.m31 = (float)mtx.mData[2].mData[0];
		res.m32 = (float)mtx.mData[2].mData[1];
		res.m33 = (float)mtx.mData[2].mData[2];
		res.m34 = (float)mtx.mData[2].mData[3];

		res.m41 = (float)mtx.mData[3].mData[0];
		res.m42 = (float)mtx.mData[3].mData[1];
		res.m43 = (float)mtx.mData[3].mData[2];
		res.m44 = (float)mtx.mData[3].mData[3];

		return res;
	}


	ImportFBXPlugin(StudioApp& _app)
		: app(_app)
		, scenes(_app.getWorldEditor()->getAllocator())
		, materials(_app.getWorldEditor()->getAllocator())
		, meshes(_app.getWorldEditor()->getAllocator())
	{
		Action* action = LUMIX_NEW(app.getWorldEditor()->getAllocator(), Action)("Import FBX", "import_fbx");
		action->func.bind<ImportFBXPlugin, &ImportFBXPlugin::toggleOpened>(this);
		action->is_selected.bind<ImportFBXPlugin, &ImportFBXPlugin::isOpened>(this);
		app.addWindowAction(action);

		fbx_manager = FbxManager::Create();
		FbxIOSettings* ios = FbxIOSettings::Create(fbx_manager, IOSROOT);
		fbx_manager->SetIOSettings(ios);

		registerLuaAPI();
	}


	static int LUA_setParams(lua_State* L)
	{
		auto* dlg = LuaWrapper::checkArg<ImportFBXPlugin*>(L, 1);
		LuaWrapper::checkTableArg(L, 2);

		if (lua_getfield(L, 2, "output_dir") == LUA_TSTRING)
		{
			dlg->output_dir = LuaWrapper::toType<const char*>(L, -1);
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 2, "scale") == LUA_TNUMBER)
		{
			dlg->mesh_scale = LuaWrapper::toType<float>(L, -1);
		}
		lua_pop(L, 1);
		return 0;
	}



	static int LUA_setMaterialParams(lua_State* L)
	{
		auto* dlg = LuaWrapper::checkArg<ImportFBXPlugin*>(L, 1);
		int material_idx = LuaWrapper::checkArg<int>(L, 2);
		LuaWrapper::checkTableArg(L, 3);
		if (material_idx < 0 || material_idx >= dlg->materials.size()) return 0;

		ImportMaterial& material = dlg->materials[material_idx];

		lua_pushvalue(L, 3);

		if (lua_getfield(L, -1, "import") == LUA_TBOOLEAN)
		{
			material.import = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1); // "import"

		if (lua_getfield(L, -1, "alpha_cutout") == LUA_TBOOLEAN)
		{
			material.alpha_cutout = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1); // "alpha_cutout"

		lua_pop(L, 1); // table
		return 0;
	}

	
	int getMaterialsCount()
	{
		return materials.size();
	}


	void registerLuaAPI()
	{
		Engine& engine = app.getWorldEditor()->getEngine();
		lua_State* L = engine.getState();

		LuaWrapper::createSystemVariable(L, "ImportFBX", "instance", this);

		#define REGISTER_FUNCTION(name) \
			do {\
				auto f = &LuaWrapper::wrapMethod<ImportFBXPlugin, decltype(&ImportFBXPlugin::name), &ImportFBXPlugin::name>; \
				LuaWrapper::createSystemFunction(L, "ImportFBX", #name, f); \
			} while(false) \

		REGISTER_FUNCTION(addSource);
		REGISTER_FUNCTION(clearSources);
		REGISTER_FUNCTION(import);
		REGISTER_FUNCTION(getMaterialsCount);

		#undef REGISTER_FUNCTION

		#define REGISTER_FUNCTION(name) \
			do {\
				LuaWrapper::createSystemFunction(L, "ImportFBX", #name, &LUA_##name); \
			} while(false) \

		REGISTER_FUNCTION(setParams);
		REGISTER_FUNCTION(setMaterialParams);

		#undef REGISTER_FUNCTION
	}


	bool addSource(const char* filename)
	{
		FbxImporter* importer = FbxImporter::Create(fbx_manager, "");

		if (!importer->Initialize(filename, -1, fbx_manager->GetIOSettings()))
		{
			error_message << "Failed to initialize fbx importer: " << importer->GetStatus().GetErrorString();
			importer->Destroy();
			return false;
		}

		FbxScene* scene = FbxScene::Create(fbx_manager, "myScene");
		if (!importer->Import(scene))
		{
			error_message << "Failed to import \"" << filename << "\": " << importer->GetStatus().GetErrorString();
			importer->Destroy();
			return false;
		}

		if (scenes.empty())
		{
			PathUtils::getBasename(output_mesh_filename.data, lengthOf(output_mesh_filename.data), filename);
		}
		FbxNode* root = scene->GetRootNode();
		gatherMaterials(root);
		gatherMeshes(root);

		scenes.push(scene);
		importer->Destroy();
		return true;
	}


	template<typename T>
	void write(const T& obj)
	{
		out_file.write(&obj, sizeof(obj));
	}


	void write(const void* ptr, size_t size)
	{
		out_file.write(ptr, size);
	}


	void writeString(const char* str)
	{
		out_file.write(str, strlen(str));
	}


	void writeMaterials(ImportMaterial* materials, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			const ImportMaterial& material = materials[i];
			if (!material.import) continue;

			StaticString<MAX_PATH_LENGTH> path(output_dir, material.fbx->GetName(), ".mat");
			IAllocator& allocator = app.getWorldEditor()->getAllocator();
			if (!out_file.open(path, FS::Mode::CREATE_AND_WRITE, allocator))
			{
				// TODO error message
				continue;
			}

			writeString("{\n\t\"shader\" : \"pipelines/rigid/rigid.shd\"");

			FbxProperty diffuse = material.fbx->FindProperty(FbxSurfaceMaterial::sDiffuse);
			FbxFileTexture* texture = diffuse.GetSrcObject<FbxFileTexture>();

			if (texture)
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"");
				PathUtils::FileInfo info(texture->GetFileName());
				writeString(info.m_basename);
				writeString(".");
				writeString(info.m_extension);
				writeString("\" } ");
			}
			else
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"\" }");
			}

			FbxProperty normal = material.fbx->FindProperty(FbxSurfaceMaterial::sNormalMap);
			texture = diffuse.GetSrcObject<FbxFileTexture>();

			if (texture)
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"");
				PathUtils::FileInfo info(texture->GetFileName());
				writeString(info.m_basename);
				writeString(".");
				writeString(info.m_extension);
				writeString("\" } ");
			}
			else
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"\" }");
			}


			if (material.fbx->GetClassId().Is(FbxSurfacePhong::ClassId))
			{
				FbxSurfacePhong* phong = (FbxSurfacePhong*)material.fbx;
			}
			else if (material.fbx->GetClassId().Is(FbxSurfaceLambert::ClassId))
			{
				FbxSurfaceLambert* lambert = (FbxSurfaceLambert*)material.fbx;
			}

			writeString("}");

			out_file.close();
		}
	}


	struct TranslationKey
	{
		Vec3 pos;
		float time;
		u16 frame;
	};


	static void compressPositions(Array<TranslationKey>& out,
		int frames,
		float sample_period,
		FbxAnimEvaluator* eval,
		FbxNode* bone,
		float error)
	// Array<aiVectorKey>& pos, const aiNodeAnim* channel, float end_time, float error)
	{
		out.clear();
		if (frames == 0) return;

		Vec3 pos = toLumixVec3(eval->GetNodeLocalTranslation(bone, FbxTimeSeconds(0)));
		TranslationKey last_written = {pos, 0, 0};
		out.push(last_written);
		if (frames == 1) return;

		float dt = sample_period;
		pos = toLumixVec3(eval->GetNodeLocalTranslation(bone, FbxTimeSeconds(sample_period)));
		Vec3 dif = (pos - last_written.pos) / sample_period;
		TranslationKey prev = {pos, sample_period, 1};
		for (u16 i = 2; i < (u16)frames; ++i)
		{
			float t = i * sample_period;
			Vec3 cur = toLumixVec3(eval->GetNodeLocalTranslation(bone, FbxTimeSeconds(t)));
			dt = t - last_written.time;
			Vec3 estimate = last_written.pos + dif * dt;
			if (fabs(estimate.x - cur.x) > error
				|| fabs(estimate.y - cur.y) > error
				|| fabs(estimate.z - cur.z) > error)
			{
				last_written = prev;
				out.push(last_written);

				dt = sample_period;
				dif = (cur - last_written.pos) / dt;
			}
			prev = {cur, t, i};
		}

		float t = frames * sample_period;
		last_written = { toLumixVec3(eval->GetNodeLocalTranslation(bone, FbxTimeSeconds(t))), t, (u16)frames};
		out.push(last_written);
	}


	void writeAnimations(FbxScene* scene, FbxNode** bones, int bone_count)
	{
		int anim_count = scene->GetSrcObjectCount<FbxAnimStack>();
		for (int i = 0; i < anim_count; ++i)
		{
			FbxAnimStack* stack = scene->GetSrcObject<FbxAnimStack>(i);
			scene->SetCurrentAnimationStack(stack);
			const char* anim_name = stack->GetName();

			FbxTimeSpan time_spawn;
			const FbxTakeInfo* take_info = scene->GetTakeInfo(stack->GetName());
			if (take_info)
			{
				time_spawn = take_info->mLocalTimeSpan;
			}
			else
			{
				scene->GetGlobalSettings().GetTimelineDefaultTimeSpan(time_spawn);
			}

			FbxTime::EMode mode = scene->GetGlobalSettings().GetTimeMode();
			float scene_frame_rate =
				static_cast<float>((mode == FbxTime::eCustom) ? scene->GetGlobalSettings().GetCustomFrameRate()
															  : FbxTime::GetFrameRate(mode));

			float sampling_period = 1.0f / scene_frame_rate;

			float start = (float)(time_spawn.GetStart().GetSecondDouble());
			float end = (float)(time_spawn.GetStop().GetSecondDouble());

			float duration = end > start ? end - start : 1.0f;

			StaticString<MAX_PATH_LENGTH> tmp(output_dir, "out.ani"); // TODO filename
			IAllocator& allocator = app.getWorldEditor()->getAllocator();
			if (!out_file.open(tmp, FS::Mode::CREATE_AND_WRITE, allocator))
			{
				// TODO error message
				continue;
			}
			Animation::Header header;
			header.magic = Animation::HEADER_MAGIC;
			header.version = 3;
			header.fps = (u32)(scene_frame_rate + 0.5f);
			write(header);

			int root_motion_bone_idx = -1;
			write(root_motion_bone_idx);
			write(int(duration / sampling_period));
			int used_bone_count = 0;

			for (int j = 0; j < bone_count; ++j)
			{
				FbxNode* bone = bones[j];
				FbxAnimEvaluator* eval = scene->GetAnimationEvaluator();
				FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>();
				FbxAnimCurveNode* curve = bone->LclTranslation.GetCurveNode(layer);
				if (curve) ++used_bone_count;
			}
			write(used_bone_count);
			Array<TranslationKey> positions(allocator);
			for (int j = 0; j < bone_count; ++j)
			{
				FbxNode* bone = bones[j];
				FbxAnimEvaluator* eval = scene->GetAnimationEvaluator();
				FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>();
				FbxAnimCurveNode* curve = bone->LclTranslation.GetCurveNode(layer);
				if (!curve) continue;
				const char* bone_name = bone->GetName();
				u32 name_hash = crc32(bone_name, (int)strlen(bone_name));
				write(name_hash);
				int frames = int((duration / sampling_period) + 0.5f);

				compressPositions(positions, frames, sampling_period, eval, bone, 0.001f);
				write(positions.size());

				for (TranslationKey& key : positions) write(key.frame);
				for (TranslationKey& key : positions)
				{
					// TODO check this in isValid function
					// assert(scale > 0.99f && scale < 1.01f);
					write(key.pos * mesh_scale);
				}
				write(frames);
				for (u16 f = 0; f < frames; ++f) write(f);
				for (u16 f = 0; f < frames; ++f)
				{
					float t = f * sampling_period;
					FbxAMatrix mtx = eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t));
					for (double d : mtx.GetQ().mData) write((float)d);
				}
			}
			out_file.close();
		}
	}


	void writeGeometry(FbxNode** bones)
	{
		IAllocator& allocator = app.getWorldEditor()->getAllocator();
		Array<Vertex> vertices(allocator);
		i32 indices_count = 0;

		for (const ImportMesh& mesh : meshes)
		{
			if(mesh.import) indices_count += mesh.fbx->GetPolygonVertexCount();
		}
		write(indices_count);
	
		for (const ImportMesh& import_mesh : meshes)
		{
			if (!import_mesh.import) continue;
			FbxMesh* mesh = import_mesh.fbx;

			struct Skin
			{
				float weights[4];
				i16 joints[4];
				int count = 0;
			};
			Array<Skin> skinning(allocator);
			skinning.resize(mesh->GetControlPointsCount());

			FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
			auto* skin = static_cast<FbxSkin*>(deformer);
			for (int i = 0; i < skin->GetClusterCount(); ++i)
			{
				FbxCluster* cluster = skin->GetCluster(i);
				int joint = indexOf(bones, cluster->GetLink());
				const int* cp_indices = cluster->GetControlPointIndices();
				const double* weights = cluster->GetControlPointWeights();
				for (int j = 0; j < cluster->GetControlPointIndicesCount(); ++j)
				{
					int idx = cp_indices[j];
					float weight = (float)weights[j];
					Skin& s = skinning[idx];
					if (s.count < 4)
					{
						s.weights[s.count] = weight;
						s.joints[s.count] = joint;
						++s.count;
					}
					else
					{
						int min = 0;
						for (int m = 1; m < 4; ++i)
						{
							if (s.weights[m] < s.weights[min]) min = m;
						}
						s.weights[min] = weight;
						s.joints[min] = joint;
					}
				}
			}

			for (Skin& s : skinning)
			{
				float sum = 0;
				for (float w : s.weights) sum += w;
				for (float& w : s.weights) w /= sum;
			}

			auto* cluster = skin->GetCluster(0);
			u16 index = 0;
			for (int i = 0, c = mesh->GetPolygonCount(); i < c; ++i)
			{
				for (int j = 0; j < mesh->GetPolygonSize(i); ++j)
				{
					write(index);
					++index;
					int vertex_index = mesh->GetPolygonVertex(i, j);
					Skin skin = skinning[vertex_index];
					Vertex v;
					FbxVector4 cp = mesh->GetControlPointAt(vertex_index);
					FbxAMatrix transform_matrix;
					FbxAMatrix geometry_matrix(
						mesh->GetNode()->GetGeometricTranslation(FbxNode::eSourcePivot),
						mesh->GetNode()->GetGeometricRotation(FbxNode::eSourcePivot),
						mesh->GetNode()->GetGeometricScaling(FbxNode::eSourcePivot));
					cluster->GetTransformMatrix(transform_matrix);
					transform_matrix *= geometry_matrix;
					// premultiply control points here, so we can constantly-scaled meshes without scale in bones
					cp = transform_matrix.MultT(cp);
					v.px = (float)cp.mData[0] * mesh_scale;
					v.py = (float)cp.mData[1] * mesh_scale;
					v.pz = (float)cp.mData[2] * mesh_scale;

					// TODO correct normal
					FbxVector4 normal;
					mesh->GetPolygonVertexNormal(i, j, normal);

					v.normal = packF4u(normal);
					bool unmapped;
					FbxVector2 uv;
					FbxStringList uv_set_name_list;
					mesh->GetUVSetNames(uv_set_name_list);
					mesh->GetPolygonVertexUV(i, j, uv_set_name_list.GetStringAt(0), uv, unmapped);
					v.u = (float)uv.mData[0];
					v.v = 1 - (float)uv.mData[1];
					memcpy(v.indices, skin.joints, sizeof(v.indices));
					memcpy(v.weights, skin.weights, sizeof(v.weights));
					vertices.push(v);
				}
			}
		}
		i32 vertices_size = (i32)(sizeof(vertices[0]) * vertices.size());
		write(vertices_size);
		write(&vertices[0], vertices_size);
	}


	void writeMeshes()
	{
		i32 mesh_count = 0;
		for (ImportMesh& mesh : meshes) if (mesh.import) ++mesh_count;
		write(mesh_count);

		i32 attr_offset = 0;
		i32 indices_offset = 0;
		for (ImportMesh& import_mesh : meshes)
		{
			if (!import_mesh.import) continue;

			FbxMesh* mesh = import_mesh.fbx;
			FbxSurfaceMaterial* material = mesh->GetNode()->GetMaterial(0);
			const char* mat = material->GetName();
			i32 mat_len = (i32)strlen(mat);
			write(mat_len);
			write(mat, strlen(mat));

			write(attr_offset);
			i32 attr_size = sizeof(Vertex) * mesh->GetPolygonCount() * 3;
			attr_offset += attr_size;
			write(attr_size);

			write(indices_offset);
			i32 mesh_tri_count = mesh->GetPolygonCount();
			indices_offset += mesh_tri_count * 3;
			write(mesh_tri_count);

			const char* name = mesh->GetName();
			if (name[0] == 0) name = mat;
			if (name[0] == 0) "Unknown";
			i32 name_len = (i32)strlen(name);
			write(name_len);
			write(name, strlen(name));
		}
	}


	void writeSkeleton(FbxNode** bones, int bone_count)
	{
		write(bone_count);

		for (int i = 0; i < bone_count; ++i)
		{
			FbxNode* node = bones[i];

			const char* name = node->GetName();
			int len = (int)strlen(name);
			write(len);
			writeString(name);

			FbxNode* parent = node->GetParent();
			if (!parent)
			{
				write((int)0);
			}
			else
			{
				const char* parent_name = parent->GetName();
				len = (int)strlen(parent_name);
				write(len);
				writeString(parent_name);
			}

			FbxMesh* mesh = getAnyMeshFromBone(node);
			FbxAMatrix tr = getBindPoseMatrix(mesh, node);

			auto q = tr.GetQ();
			auto t = tr.GetT();
			for (int i = 0; i < 3; ++i) write((float)tr.GetT().mData[i] * mesh_scale);
			for (double d : tr.GetQ().mData) write((float)d);
		}
	}


	void writeLODs()
	{
		i32 lod_count = 1;
		write(lod_count);
		i32 to_mesh = meshes.size() - 1;
		write(to_mesh);
		float lod = FLT_MAX;
		write(lod);
	}


	void writeModelHeader()
	{
		Model::FileHeader header;
		header.magic = 0x5f4c4d4f; // == '_LMO';
		header.version = (u32)Model::FileVersion::LATEST;
		write(header);
		u32 flags = (u32)Model::Flags::INDICES_16BIT;
		write(flags);

		i32 attribute_count = 5;
		write(attribute_count);

		i32 pos_attr = 0;
		write(pos_attr);
		i32 nrm_attr = 1;
		write(nrm_attr);
		i32 uv0_attr = 8;
		write(uv0_attr);
		i32 indices_attr = 6;
		write(indices_attr);
		i32 weight_attr = 7;
		write(weight_attr);
	}


	bool import()
	{
		StaticString<MAX_PATH_LENGTH> out_path(output_dir, output_mesh_filename, ".msh");
		if (!out_file.open(out_path, FS::Mode::CREATE_AND_WRITE, app.getWorldEditor()->getAllocator()))
		{
			error_message << "Failed to create " << out_path;
			return false;
		}
		
		IAllocator& allocator = app.getWorldEditor()->getAllocator();
		Array<FbxNode*> bones(allocator);
		for (FbxScene* scene : scenes)
		{
			FbxNode* root = scene->GetRootNode();
			gatherBones(root, bones);
		}

		writeModelHeader();
		writeMeshes();
		writeGeometry(&bones[0]);
		writeSkeleton(&bones[0], (int)bones.size());
		writeLODs();
		out_file.close();

		ASSERT(scenes.size() == 1); // only 1 scene supported ATM
		writeAnimations(scenes[0], &bones[0], bones.size());

		writeMaterials(&materials[0], materials.size());

		return true;
	}


	void clearSources()
	{
		for (FbxScene* scene : scenes) scene->Destroy();
		scenes.clear();
		materials.clear();
	}


	~ImportFBXPlugin()
	{
		fbx_manager->Destroy();
	}


	void toggleOpened() { opened = !opened; }
	bool isOpened() const { return opened; }


	void onMeshesGUI()
	{
		StaticString<30> label("Meshes (");
		label << meshes.size() << ")###Meshes";
		if (!ImGui::CollapsingHeader(label)) return;

		ImGui::InputText("Output mesh filename", output_mesh_filename.data, sizeof(output_mesh_filename.data));

		ImGui::Indent();
		ImGui::Columns(5);

		ImGui::Text("Mesh");
		ImGui::NextColumn();
		ImGui::Text("Material");
		ImGui::NextColumn();
		ImGui::Text("Import mesh");
		ImGui::NextColumn();
		ImGui::Text("Import physics");
		ImGui::NextColumn();
		ImGui::Text("LOD");
		ImGui::NextColumn();
		ImGui::Separator();

		for (auto& mesh : meshes)
		{
			const char* name = mesh.fbx->GetName();
			FbxSurfaceMaterial* material = mesh.fbx->GetNode()->GetMaterial(0);
			if (name[0] == '\0' && material) name = material->GetName();
			ImGui::Text("%s", name);
			ImGui::NextColumn();

			// TODO
			/*auto* material = mesh.scene->mMaterials[mesh.mesh->mMaterialIndex];
			aiString material_name;
			material->Get(AI_MATKEY_NAME, material_name);
			ImGui::Text("%s", material_name.C_Str());*/
			ImGui::NextColumn();

			ImGui::Checkbox(StaticString<30>("###mesh", (u64)&mesh), &mesh.import);
			if (ImGui::GetIO().MouseClicked[1] && ImGui::IsItemHovered()) ImGui::OpenPopup("ContextMesh");
			ImGui::NextColumn();
			ImGui::Checkbox(StaticString<30>("###phy", (u64)&mesh), &mesh.import_physics);
			if (ImGui::GetIO().MouseClicked[1] && ImGui::IsItemHovered()) ImGui::OpenPopup("ContextPhy");
			ImGui::NextColumn();
			ImGui::Combo(StaticString<30>("###lod", (u64)&mesh), &mesh.lod, "LOD 1\0LOD 2\0LOD 3\0LOD 4\0");
			ImGui::NextColumn();
		}
		ImGui::Columns();
		ImGui::Unindent();
		if (ImGui::BeginPopup("ContextMesh"))
		{
			if (ImGui::Selectable("Select all"))
			{
				for (auto& mesh : meshes) mesh.import = true;
			}
			if (ImGui::Selectable("Deselect all"))
			{
				for (auto& mesh : meshes) mesh.import = false;
			}
			ImGui::EndPopup();
		}
		if (ImGui::BeginPopup("ContextPhy"))
		{
			if (ImGui::Selectable("Select all"))
			{
				for (auto& mesh : meshes) mesh.import_physics = true;
			}
			if (ImGui::Selectable("Deselect all"))
			{
				for (auto& mesh : meshes) mesh.import_physics = false;
			}
			ImGui::EndPopup();
		}
	}


	void onMaterialsGUI()
	{
		StaticString<30> label("Materials (");
		label << materials.size() << ")###Materials";
		if (!ImGui::CollapsingHeader(label)) return;

		ImGui::Indent();
		if (ImGui::Button("Import all materials"))
		{
			for (auto& mat : materials) mat.import = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Do not import any materials"))
		{
			for (auto& mat : materials) mat.import = false;
		}

		for (auto& mat : materials)
		{
			if (ImGui::TreeNode(mat.fbx, "%s", mat.fbx->GetName()))
			{
				ImGui::Checkbox("Import material", &mat.import);
				ImGui::SameLine();
				ImGui::Checkbox("Alpha cutout material", &mat.alpha_cutout);
				// TODO
				/*
				ImGui::Columns(4);
				ImGui::Text("Path");
				ImGui::NextColumn();
				ImGui::Text("Import");
				ImGui::NextColumn();
				ImGui::Text("Convert to DDS");
				ImGui::NextColumn();
				ImGui::Text("Source");
				ImGui::NextColumn();
				ImGui::Separator();
				for (int i = 0; i < mat.texture_count; ++i)
				{
					ImGui::Text("%s", mat.textures[i].path);
					ImGui::NextColumn();
					ImGui::Checkbox(StaticString<20>("###imp", i), &mat.textures[i].import);
					ImGui::NextColumn();
					ImGui::Checkbox(StaticString<20>("###dds", i), &mat.textures[i].to_dds);
					ImGui::NextColumn();
					if (ImGui::Button(StaticString<50>("Browse###brw", i)))
					{
						if (PlatformInterface::getOpenFilename(
							mat.textures[i].src, lengthOf(mat.textures[i].src), "All\0*.*\0", nullptr))
						{
							mat.textures[i].is_valid = true;
						}
					}
					ImGui::SameLine();
					ImGui::Text("%s", mat.textures[i].src);
					ImGui::NextColumn();
				}
				ImGui::Columns();*/

				ImGui::TreePop();
			}
		}
		ImGui::Unindent();
	}


	void onWindowGUI() override
	{
		if (ImGui::BeginDock("Import FBX", &opened))
		{
			if (ImGui::Button("Add source"))
			{
				char src_path[MAX_PATH_LENGTH];
				if (PlatformInterface::getOpenFilename(src_path, lengthOf(src_path), "All\0*.*\0", nullptr))
				{
					addSource(src_path);
				}
			}

			if (!scenes.empty())
			{
				ImGui::SameLine();
				if (ImGui::Button("Clear sources")) clearSources();
				
				onMeshesGUI();
				onMaterialsGUI();

				ImGui::InputFloat("Scale", &mesh_scale);
				ImGui::InputText("Output directory", output_dir.data, sizeof(output_dir));
				ImGui::SameLine();
				if (ImGui::Button("...###browseoutput"))
				{
					if (PlatformInterface::getOpenDirectory(output_dir.data, sizeof(output_dir), last_dir))
					{
						last_dir = output_dir;
					}
				}

				if (ImGui::Button("Convert")) import();
			}
		}
		ImGui::EndDock();
	}


	const char* getName() const override { return "import_fbx"; }


	StudioApp& app;
	bool opened = false;
	FbxManager* fbx_manager = nullptr;
	Array<ImportMaterial> materials;
	Array<ImportMesh> meshes;
	StaticString<1024> error_message;
	StaticString<MAX_PATH_LENGTH> output_dir;
	StaticString<MAX_PATH_LENGTH> last_dir;
	StaticString<MAX_PATH_LENGTH> output_mesh_filename;
	Array<FbxScene*> scenes;
	FS::OsFile out_file;
	float mesh_scale = 1.0f;
};


LUMIX_STUDIO_ENTRY(lumixengine_fbx)
{
	auto& editor = *app.getWorldEditor();
	auto* plugin = LUMIX_NEW(editor.getAllocator(), ImportFBXPlugin)(app);
	app.addPlugin(*plugin);
}


} // namespace Lumix

