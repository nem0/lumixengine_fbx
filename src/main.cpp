#include "animation/animation.h"
#include "editor/platform_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/fs/disk_file_device.h"
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
	struct ImportAnimation
	{
		FbxAnimStack* fbx = nullptr;
		StaticString<MAX_PATH_LENGTH> output_filename;
		bool import = true;
	};

	struct ImportMaterial
	{
		FbxSurfaceMaterial* fbx = nullptr;
		bool import = true;
		bool alpha_cutout = false;
	};

	struct ImportMesh
	{
		FbxMesh* fbx = nullptr;
		FbxSurfaceMaterial* fbx_mat = nullptr;
		bool import = true;
		bool import_physics = false;
		int lod = 0;
	};

	static u32 packu32(u8 _x, u8 _y, u8 _z, u8 _w)
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


	static u32 packF4u(const Vec3& vec)
	{
		const u8 xx = u8(vec.x * 127.0f + 128.0f);
		const u8 yy = u8(vec.y * 127.0f + 128.0f);
		const u8 zz = u8(vec.z * 127.0f + 128.0f);
		const u8 ww = u8(0);
		return packu32(xx, yy, zz, ww);
	}


	FbxMesh* getAnyMeshFromBone(FbxNode* node) const
	{
		for (int i = 0; i < meshes.size(); ++i)
		{
			FbxMesh* mesh = meshes[i].fbx;
			if (mesh->GetDeformerCount(FbxDeformer::EDeformerType::eSkin) <= 0) continue;
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

		FbxAMatrix transform_link_matrix;
		cluster->GetTransformLinkMatrix(transform_link_matrix);
		return transform_link_matrix;
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


	static void insertHierarchy(Array<FbxNode*>& bones, FbxNode* node)
	{
		if (!node) return;
		if (bones.indexOf(node) >= 0) return;
		insertHierarchy(bones, node->GetParent());
		bones.push(node);
	}


	void gatherBones(FbxNode* node)
	{
		const FbxNodeAttribute* node_attr = node->GetNodeAttribute();
		bool is_bone = node_attr && node_attr->GetAttributeType() == FbxNodeAttribute::EType::eSkeleton;

		if (is_bone) insertHierarchy(bones, node);

		for (int i = 0; i < node->GetChildCount(); ++i)
		{
			gatherBones(node->GetChild(i));
		}
	}


	void gatherAnimations(FbxScene* scene)
	{
		int anim_count = scene->GetSrcObjectCount<FbxAnimStack>();
		for (int i = 0; i < anim_count; ++i)
		{
			ImportAnimation& anim = animations.emplace();
			anim.fbx = scene->GetSrcObject<FbxAnimStack>(i);
			anim.import = true;
			
			const FbxTakeInfo* take_info = scene->GetTakeInfo(anim.fbx->GetName());
			if (take_info)
			{
				if (!take_info->mName.IsEmpty()) anim.output_filename = take_info->mName.Buffer();
				if (anim.output_filename.empty() && !take_info->mImportName.IsEmpty()) anim.output_filename = take_info->mImportName.Buffer();
				if (anim.output_filename.empty()) anim.output_filename << "anim";
			}
			else
			{
				anim.output_filename = "anim";
			}
			anim.output_filename = anim.fbx->GetName();
		}
	}


	void gatherMeshes(FbxScene* scene)
	{
		int c = scene->GetSrcObjectCount<FbxMesh>();
		for (int i = 0; i < c; ++i)
		{
			ImportMesh& mesh = meshes.emplace();
			mesh.fbx = scene->GetSrcObject<FbxMesh>(i);
			mesh.lod = detectMeshLOD(mesh);
			if (mesh.fbx->GetElementMaterialCount() == 0) continue;

			const auto& index_array = mesh.fbx->GetElementMaterial(0)->GetIndexArray();
			if (index_array.GetCount() == 0) continue;

			FbxNode* node = mesh.fbx->GetNode();
			mesh.fbx_mat = node->GetMaterial(index_array[0]);
		}
	}


	static int detectMeshLOD(const ImportMesh& mesh)
	{
		const char* node_name = mesh.fbx->GetNode()->GetName();
		const char* lod_str = stristr(node_name, "_LOD");
		if (!lod_str)
		{ 
			const char* mesh_name = getImportMeshName(mesh);
			if (!mesh_name) return 0;

			const char* lod_str = stristr(mesh_name, "_LOD");
			if (!lod_str) return 0;
		}

		lod_str += stringLength("_LOD");

		int lod;
		fromCString(lod_str, stringLength(lod_str), &lod);

		return lod;
	}


	static bool isValid(FbxMesh** meshes, int mesh_count)
	{
		// TODO call this function
		// TODO error message
		// TODO check is there are not the same bones in multiple scenes
		// TODO check if all meshes have the same vertex decls
		for (int i = 0; i < mesh_count; ++i)
		{
			FbxMesh* mesh = meshes[i];
			if (!mesh->IsTriangleMesh()) return false;
			if (mesh->GetDeformerCount() > 1) return false;
		}
		return true;
	}


	static Vec3 toLumixVec3(const FbxVector4& v) { return{ (float)v.mData[0], (float)v.mData[1], (float)v.mData[2] }; }
	static Quat toLumix(const FbxQuaternion& q)
	{
		return {(float)q.mData[0], (float)q.mData[1], (float)q.mData[2], (float)q.mData[3]};
	}

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
		, animations(_app.getWorldEditor()->getAllocator())
		, bones(_app.getWorldEditor()->getAllocator())
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
		auto* dlg= LuaWrapper::toType<ImportFBXPlugin*>(L, lua_upvalueindex(1));
		LuaWrapper::checkTableArg(L, 1);

		if (lua_getfield(L, 1, "lods") == LUA_TTABLE)
		{
			lua_pushnil(L);
			int lod_index = 0;
			while (lua_next(L, -2) != 0)
			{
				if (lod_index >= lengthOf(dlg->lods_distances))
				{
					g_log_error.log("Editor") << "Only " << lengthOf(dlg->lods_distances) << " supported";
					lua_pop(L, 1);
					break;
				}

				dlg->lods_distances[lod_index] = LuaWrapper::toType<float>(L, -1);
				++lod_index;
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1); // "lods"
		if (lua_getfield(L, 1, "output_dir") == LUA_TSTRING)
		{
			dlg->output_dir = LuaWrapper::toType<const char*>(L, -1);
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 1, "texture_dir") == LUA_TSTRING)
		{
			dlg->texture_dir = LuaWrapper::toType<const char*>(L, -1);
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 1, "to_dds") == LUA_TBOOLEAN)
		{
			dlg->to_dds = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 1, "orientation") == LUA_TSTRING)
		{
			const char* tmp = LuaWrapper::toType<const char*>(L, -1);
			if (equalStrings(tmp, "+y")) dlg->orientation = Orientation::Y_UP;
			else if (equalStrings(tmp, "+z")) dlg->orientation = Orientation::Z_UP;
			else if (equalStrings(tmp, "-y")) dlg->orientation = Orientation::X_MINUS_UP;
			else if (equalStrings(tmp, "-z")) dlg->orientation = Orientation::Z_MINUS_UP;
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 1, "center_mesh") == LUA_TBOOLEAN)
		{
			dlg->center_mesh = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1);
		if (lua_getfield(L, 1, "scale") == LUA_TNUMBER)
		{
			dlg->mesh_scale = LuaWrapper::toType<float>(L, -1);
		}
		lua_pop(L, 1);
		return 0;
	}

	static int LUA_setAnimationParams(lua_State* L)
	{
		auto* dlg = LuaWrapper::toType<ImportFBXPlugin*>(L, lua_upvalueindex(1));
		int anim_idx = LuaWrapper::checkArg<int>(L, 1);
		LuaWrapper::checkTableArg(L, 2);
		if (anim_idx < 0 || anim_idx >= dlg->animations.size()) return 0;

		ImportAnimation& anim = dlg->animations[anim_idx];

		lua_pushvalue(L, 2);

		if (lua_getfield(L, -1, "import") == LUA_TBOOLEAN)
		{
			anim.import = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1); // "import"

		if (lua_getfield(L, -1, "filename") == LUA_TSTRING)
		{
			anim.output_filename = LuaWrapper::toType<const char*>(L, -1);
		}
		lua_pop(L, 1); // "import"

		lua_pop(L, 1); // table
		return 0;
	}


	static int LUA_setMeshParams(lua_State* L)
	{
		auto* dlg = LuaWrapper::toType<ImportFBXPlugin*>(L, lua_upvalueindex(1));
		int mesh_idx = LuaWrapper::checkArg<int>(L, 1);
		LuaWrapper::checkTableArg(L, 2);
		if (mesh_idx < 0 || mesh_idx >= dlg->meshes.size()) return 0;

		ImportMesh& mesh = dlg->meshes[mesh_idx];

		lua_pushvalue(L, 2);

		if (lua_getfield(L, -1, "import") == LUA_TBOOLEAN)
		{
			mesh.import = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1); // "import"

		if (lua_getfield(L, -1, "import_physics") == LUA_TBOOLEAN)
		{
			mesh.import_physics = LuaWrapper::toType<bool>(L, -1);
		}
		lua_pop(L, 1); // "import_physics"

		if (lua_getfield(L, -1, "lod") == LUA_TNUMBER)
		{
			mesh.lod = LuaWrapper::toType<int>(L, -1);
		}
		lua_pop(L, 1); // "lod"

		lua_pop(L, 1); // table
		return 0;
	}


	static int LUA_setMaterialParams(lua_State* L)
	{
		auto* dlg = LuaWrapper::toType<ImportFBXPlugin*>(L, lua_upvalueindex(1));
		int material_idx = LuaWrapper::checkArg<int>(L, 1);
		LuaWrapper::checkTableArg(L, 2);
		if (material_idx < 0 || material_idx >= dlg->materials.size()) return 0;

		ImportMaterial& material = dlg->materials[material_idx];

		lua_pushvalue(L, 2);

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


	int getAnimationsCount() { return animations.size(); }
	int getMaterialsCount() { return materials.size(); }
	int getMeshesCount() { return meshes.size(); }
	const char* getMeshName(int mesh_idx) { return getImportMeshName(meshes[mesh_idx]); }
	int getMeshLOD(int mesh_idx) { return meshes[mesh_idx].lod; }


	void registerLuaAPI()
	{
		Engine& engine = app.getWorldEditor()->getEngine();
		lua_State* L = engine.getState();

		LuaWrapper::createSystemVariable(L, "ImportFBX", "instance", this);

		#define REGISTER_FUNCTION(name) \
			do {\
				auto f = &LuaWrapper::wrapMethodClosure<ImportFBXPlugin, decltype(&ImportFBXPlugin::name), &ImportFBXPlugin::name>; \
				LuaWrapper::createSystemClosure(L, "ImportFBX", this, #name, f); \
			} while(false) \

		REGISTER_FUNCTION(addSource);
		REGISTER_FUNCTION(clearSources);
		REGISTER_FUNCTION(import);
		REGISTER_FUNCTION(getMaterialsCount);
		REGISTER_FUNCTION(getAnimationsCount);
		REGISTER_FUNCTION(getMeshesCount);
		REGISTER_FUNCTION(getMeshName);
		REGISTER_FUNCTION(getMeshLOD);

		#undef REGISTER_FUNCTION

		#define REGISTER_FUNCTION(name) \
			do {\
				LuaWrapper::createSystemClosure(L, "ImportFBX", this, #name, &LUA_##name); \
			} while(false) \

		REGISTER_FUNCTION(setParams);
		REGISTER_FUNCTION(setMaterialParams);
		REGISTER_FUNCTION(setMeshParams);
		REGISTER_FUNCTION(setAnimationParams);

		#undef REGISTER_FUNCTION
	}


	bool addSource(const char* filename)
	{
		FbxImporter* importer = FbxImporter::Create(fbx_manager, "");

		if (!importer->Initialize(filename, -1, fbx_manager->GetIOSettings()))
		{
			g_log_error.log("FBX") << "Failed to initialize fbx importer: " << importer->GetStatus().GetErrorString();
			importer->Destroy();
			return false;
		}

		FbxScene* scene = FbxScene::Create(fbx_manager, "myScene");
		if (!importer->Import(scene))
		{
			g_log_error.log("FBX") << "Failed to import \"" << filename << "\": " << importer->GetStatus().GetErrorString();
			importer->Destroy();
			return false;
		}

		FbxGeometryConverter converter(fbx_manager);
		converter.SplitMeshesPerMaterial(scene, true);
		converter.Triangulate(scene, true);

		if (scenes.empty())
		{
			PathUtils::getBasename(output_mesh_filename.data, lengthOf(output_mesh_filename.data), filename);
		}

		FbxNode* root = scene->GetRootNode();
		gatherMaterials(root);
		materials.removeDuplicates([](const ImportMaterial& a, const ImportMaterial& b) { return a.fbx == b.fbx; });
		gatherMeshes(scene);
		gatherBones(root);
		gatherAnimations(scene);

		scenes.push(scene);
		importer->Destroy();
		return true;
	}


	template <typename T> void write(const T& obj) { out_file.write(&obj, sizeof(obj)); }
	void write(const void* ptr, size_t size) { out_file.write(ptr, size); }
	void writeString(const char* str) { out_file.write(str, strlen(str)); }


	void writeMaterials()
	{
		for (const ImportMaterial& material : materials)
		{
			if (!material.import) continue;

			StaticString<MAX_PATH_LENGTH> path(output_dir, material.fbx->GetName(), ".mat");
			IAllocator& allocator = app.getWorldEditor()->getAllocator();
			if (!out_file.open(path, FS::Mode::CREATE_AND_WRITE, allocator))
			{
				g_log_error.log("FBX") << "Failed to create " << path;
				continue;
			}

			writeString("{\n\t\"shader\" : \"pipelines/rigid/rigid.shd\"");
			if (material.alpha_cutout) writeString(",\n\t\"defines\" : [\"ALPHA_CUTOUT\"]");
			auto writeTexture = [this](FbxFileTexture* texture, bool srgb) {
				if (texture)
				{
					writeString(",\n\t\"texture\" : { \"source\" : \"");
					PathUtils::FileInfo info(texture->GetFileName());
					writeString(texture_dir.data);
					writeString(info.m_basename);
					writeString(".");
					writeString(to_dds ? "dds" : info.m_extension);
					writeString("\"");
					if(srgb) writeString(", \"srgb\" : true ");
					writeString("}");
				}
				else
				{
					writeString(",\n\t\"texture\" : {");
					if (srgb) writeString(" \"srgb\" : true ");
					writeString("}");
				}
			};

			FbxProperty diffuse = material.fbx->FindProperty(FbxSurfaceMaterial::sDiffuse);
			FbxFileTexture* texture = diffuse.GetSrcObject<FbxFileTexture>();
			writeTexture(texture, true);

			FbxProperty normal = material.fbx->FindProperty(FbxSurfaceMaterial::sNormalMap);
			texture = normal.GetSrcObject<FbxFileTexture>();
			writeTexture(texture, false);

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
	{
		out.clear();
		if (frames == 0) return;

		Vec3 pos = toLumixVec3(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(0)).GetT());
		TranslationKey last_written = {pos, 0, 0};
		out.push(last_written);
		if (frames == 1) return;

		float dt = sample_period;
		pos = toLumixVec3(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(sample_period)).GetT());
		Vec3 dif = (pos - last_written.pos) / sample_period;
		TranslationKey prev = {pos, sample_period, 1};
		for (u16 i = 2; i < (u16)frames; ++i)
		{
			float t = i * sample_period;
			Vec3 cur = toLumixVec3(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t)).GetT());
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
		last_written = { toLumixVec3(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t)).GetT()), t, (u16)frames};
		out.push(last_written);
	}


	struct RotationKey
	{
		Quat rot;
		float time;
		u16 frame;
	};


	static void compressRotations(Array<RotationKey>& out,
		int frames,
		float sample_period,
		FbxAnimEvaluator* eval,
		FbxNode* bone,
		float error)
	{
		out.clear();
		if (frames == 0) return;

		Quat rot = toLumix(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(0)).GetQ());
		RotationKey last_written = {rot, 0, 0};
		out.push(last_written);
		if (frames == 1) return;

		float dt = sample_period;
		rot = toLumix(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(sample_period)).GetQ());
		RotationKey after_last = {rot, sample_period, 1};
		RotationKey prev = after_last;
		for (u16 i = 2; i < (u16)frames; ++i)
		{
			float t = i * sample_period;
			Quat cur = toLumix(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t)).GetQ());
			Quat estimate;
			nlerp(cur, last_written.rot, &estimate, sample_period / (t - last_written.time));
			if (fabs(estimate.x - after_last.rot.x) > error || fabs(estimate.y - after_last.rot.y) > error ||
				fabs(estimate.z - after_last.rot.z) > error)
			{
				last_written = prev;
				out.push(last_written);

				after_last = {cur, t, i};
			}
			prev = {cur, t, i};
		}

		float t = frames * sample_period;
		last_written = {toLumix(eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t)).GetQ()), t, (u16)frames};
		out.push(last_written);
	}


	void writeAnimations()
	{
		for (ImportAnimation& anim : animations)
		{
			if (!anim.import) continue;

			FbxAnimStack* stack = anim.fbx;
			FbxScene* scene = stack->GetScene();
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
				(float)((mode == FbxTime::eCustom) ? scene->GetGlobalSettings().GetCustomFrameRate()
												   : FbxTime::GetFrameRate(mode));

			float sampling_period = 1.0f / scene_frame_rate;

			float start = (float)(time_spawn.GetStart().GetSecondDouble());
			float end = (float)(time_spawn.GetStop().GetSecondDouble());

			float duration = end > start ? end - start : 1.0f;

			StaticString<MAX_PATH_LENGTH> tmp(output_dir, anim.output_filename, ".ani");
			IAllocator& allocator = app.getWorldEditor()->getAllocator();
			if (!out_file.open(tmp, FS::Mode::CREATE_AND_WRITE, allocator))
			{
				g_log_error.log("FBX") << "Failed to create " << tmp;
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

			for (FbxNode* bone : bones)
			{
				if (bone->GetScene() != scene) continue;
				
				FbxAnimEvaluator* eval = scene->GetAnimationEvaluator();
				FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>();
				FbxAnimCurveNode* curve = bone->LclTranslation.GetCurveNode(layer);
				if (curve) ++used_bone_count;
			}
			write(used_bone_count);
			Array<TranslationKey> positions(allocator);
			Array<RotationKey> rotations(allocator);
			for (FbxNode* bone : bones)
			{
				if (bone->GetScene() != scene) continue;

				FbxAnimEvaluator* eval = scene->GetAnimationEvaluator();
				FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>();
				FbxAnimCurveNode* curve = bone->LclTranslation.GetCurveNode(layer);
				if (!curve) continue;
				
				u32 name_hash = crc32(bone->GetName());
				write(name_hash);
				int frames = int((duration / sampling_period) + 0.5f);

				compressPositions(positions, frames, sampling_period, eval, bone, 0.001f);
				write(positions.size());

				for (TranslationKey& key : positions) write(key.frame);
				for (TranslationKey& key : positions)
				{
					// TODO check this in isValid function
					// assert(scale > 0.99f && scale < 1.01f);
					write(fixOrientation(key.pos * mesh_scale));
				}

				compressRotations(rotations, frames, sampling_period, eval, bone, 0.0001f);

				write(rotations.size());
				for (RotationKey& key : rotations) write(key.frame);
				for (RotationKey& key : rotations) write(fixOrientation(key.rot));
			}
			out_file.close();
		}
	}


	static bool isSkinned(FbxMesh* mesh)
	{
		return mesh->GetDeformerCount(FbxDeformer::EDeformerType::eSkin) > 0;
	}


	static int getVertexSize(FbxMesh* mesh)
	{
		static const int POSITION_SIZE = sizeof(float) * 3;
		static const int NORMAL_SIZE = sizeof(u8) * 4;
		static const int TANGENT_SIZE = sizeof(u8) * 4;
		static const int UV_SIZE = sizeof(float) * 2;
		static const int COLOR_SIZE = sizeof(u8) * 4;
		static const int BONE_INDICES_WEIGHTS_SIZE = sizeof(float) * 4 + sizeof(u16) * 4;
		int size = POSITION_SIZE + NORMAL_SIZE;

		// TODO
		//if (mesh->GetElementTangentCount() > 0) size += TANGENT_SIZE;
		if (mesh->GetElementUVCount() > 0) size += UV_SIZE;
		// TODO
		//if (mesh->GetElementVertexColorCount() > 0) size += COLOR_SIZE;
		if (isSkinned(mesh)) size += BONE_INDICES_WEIGHTS_SIZE;

		return size;
	}

	struct Skin
	{
		float weights[4];
		i16 joints[4];
		int count = 0;
	};


	void fillSkinInfo(Array<Skin>& skinning, const FbxMesh* mesh)const 
	{
		skinning.resize(mesh->GetControlPointsCount());

		FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
		auto* skin = static_cast<FbxSkin*>(deformer);
		for (int i = 0; i < skin->GetClusterCount(); ++i)
		{
			FbxCluster* cluster = skin->GetCluster(i);
			int joint = bones.indexOf(cluster->GetLink());
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
					for (int m = 1; m < 4; ++m)
					{
						if (s.weights[m] < s.weights[min]) min = m;
					}

					if (s.weights[min] < weight)
					{
						s.weights[min] = weight;
						s.joints[min] = joint;
					}
				}
			}
		}

		for (Skin& s : skinning)
		{
			float sum = 0;
			for (float w : s.weights) sum += w;
			for (float& w : s.weights) w /= sum;
		}
	}


	Vec3 fixOrientation(const Vec3& v) const
	{
		switch (orientation)
		{
			case Orientation::Y_UP: return Vec3(v.x, v.y, v.z);
			case Orientation::Z_UP: return Vec3(v.x, v.z, -v.y);
			case Orientation::Z_MINUS_UP: return Vec3(v.x, -v.z, v.y);
			case Orientation::X_MINUS_UP: return Vec3(v.y, -v.x, v.z);
		}
		ASSERT(false);
		return Vec3(v.x, v.y, v.z);
	}


	Quat fixOrientation(const Quat& v) const
	{
		switch (orientation)
		{
			case Orientation::Y_UP: return Quat(v.x, v.y, v.z, v.w);
			case Orientation::Z_UP: return Quat(v.x, v.z, -v.y, v.w);
			case Orientation::Z_MINUS_UP: return Quat(v.x, -v.z, v.y, v.w);
			case Orientation::X_MINUS_UP: return Quat(v.y, -v.x, v.z, v.w);
		}
		ASSERT(false);
		return Quat(v.x, v.y, v.z, v.w);
	}


	// TODO mesh is 4times the size of assimp
	void writeGeometry()
	{
		i32 indices_count = 0;
		IAllocator& allocator = app.getWorldEditor()->getAllocator();

		for (const ImportMesh& mesh : meshes)
		{
			if(mesh.import) indices_count += mesh.fbx->GetPolygonVertexCount();
		}
		write(indices_count);
	
		OutputBlob vertices_blob(allocator);
		for (const ImportMesh& import_mesh : meshes)
		{
			if (!import_mesh.import) continue;

			Array<Skin> skinning(allocator);
			FbxMesh* mesh = import_mesh.fbx;
			bool is_skinned = isSkinned(mesh);

			Matrix transform_matrix = Matrix::IDENTITY;
			FbxNode* mesh_node = mesh->GetNode();
			FbxAMatrix geometry_matrix(
				mesh_node->GetGeometricTranslation(FbxNode::eSourcePivot),
				mesh_node->GetGeometricRotation(FbxNode::eSourcePivot),
				mesh_node->GetGeometricScaling(FbxNode::eSourcePivot));
			if (is_skinned)
			{
				fillSkinInfo(skinning, mesh);

				FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
				auto* skin = static_cast<FbxSkin*>(deformer);
				auto* cluster = skin->GetCluster(0);
				FbxAMatrix mtx;
				cluster->GetTransformMatrix(mtx);
				mtx *= geometry_matrix;
				transform_matrix = toLumix(mtx);
			}
			else
			{
				FbxAMatrix node_global_mtx = mesh_node->EvaluateGlobalTransform();
				transform_matrix = toLumix(node_global_mtx * geometry_matrix);
				if (center_mesh)
				{
					transform_matrix.setTranslation({0, 0, 0});
				}
			}
			bool has_uvs = mesh->GetElementUVCount() > 0;
			FbxStringList uv_set_name_list;
			const char* uv_set_name = nullptr;
			if (has_uvs)
			{
				mesh->GetUVSetNames(uv_set_name_list);
				uv_set_name = uv_set_name_list.GetStringAt(0);
			}
			u16 index = 0;
			for (int i = 0, c = mesh->GetPolygonCount(); i < c; ++i)
			{
				for (int j = 0; j < mesh->GetPolygonSize(i); ++j)
				{
					write(index);
					++index;
					int vertex_index = mesh->GetPolygonVertex(i, j);
					FbxVector4 cp = mesh->GetControlPointAt(vertex_index);
					// premultiply control points here, so we can have constantly-scaled meshes without scale in bones
					Vec3 pos = transform_matrix.transform(toLumixVec3(cp)) * mesh_scale;
					pos = fixOrientation(pos);
					vertices_blob.write(pos);

					FbxVector4 fbx_normal;
					mesh->GetPolygonVertexNormal(i, j, fbx_normal);
					Vec3 normal = toLumixVec3(fbx_normal);
					normal = transform_matrix * Vec4(normal, 0);
					normal.normalize();
					normal = fixOrientation(normal);

					u32 packed_normal = packF4u(normal);
					vertices_blob.write(packed_normal);
					if (has_uvs)
					{
						bool unmapped;
						FbxVector2 uv;
						mesh->GetPolygonVertexUV(i, j, uv_set_name, uv, unmapped);
						Vec2 tex_cooords = {(float)uv.mData[0], 1 - (float)uv.mData[1]};
						vertices_blob.write(tex_cooords);
					}
					if (is_skinned)
					{
						Skin skin = skinning[vertex_index];
						vertices_blob.write(skin.joints);
						vertices_blob.write(skin.weights);
					}
				}
			}
		}
		write(vertices_blob.getPos());
		write(vertices_blob.getData(), vertices_blob.getPos());
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
			FbxSurfaceMaterial* material = import_mesh.fbx_mat;
			const char* mat = material->GetName();
			i32 mat_len = (i32)strlen(mat);
			write(mat_len);
			write(mat, strlen(mat));

			write(attr_offset);
			i32 attr_size = getVertexSize(mesh) * mesh->GetPolygonCount() * 3;
			attr_offset += attr_size;
			write(attr_size);

			write(indices_offset);
			i32 mesh_tri_count = mesh->GetPolygonCount();
			indices_offset += mesh_tri_count * 3;
			write(mesh_tri_count);

			const char* name = getImportMeshName(import_mesh);
			i32 name_len = (i32)strlen(name);
			write(name_len);
			write(name, strlen(name));
		}
	}


	void writeSkeleton()
	{
		write(bones.size());

		for (FbxNode* node : bones)
		{
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

			Quat q = fixOrientation(toLumix(tr.GetQ()));
			Vec3 t = fixOrientation(toLumixVec3(tr.GetT()));
			write(t * mesh_scale);
			write(q);
		}
	}


	void writeLODs()
	{
		i32 lod_count = 1;
		i32 last_mesh_idx = -1;
		i32 lods[8] = {};
		for (auto& mesh : meshes)
		{
			if (!mesh.import) continue;

			++last_mesh_idx;
			if (mesh.lod >= lengthOf(lods_distances)) continue;
			lod_count = mesh.lod + 1;
			lods[mesh.lod] = last_mesh_idx;
		}

		for (int i = 1; i < Lumix::lengthOf(lods); ++i)
		{
			if (lods[i] < lods[i - 1]) lods[i] = lods[i - 1];
		}

		write((const char*)&lod_count, sizeof(lod_count));

		for (int i = 0; i < lod_count; ++i)
		{
			i32 to_mesh = lods[i];
			write((const char*)&to_mesh, sizeof(to_mesh));
			float factor = lods_distances[i] < 0 ? FLT_MAX : lods_distances[i] * lods_distances[i];
			write((const char*)&factor, sizeof(factor));
		}
	
	}


	static int getAttributeCount(FbxMesh* mesh)
	{
		int count = 2; // position, normal
		if (mesh->GetElementUVCount() > 0) ++count;
		if (isSkinned(mesh)) count += 2;
		// TODO
		//if (mesh->HasVertexColors(0) && m_dialog.m_model.import_vertex_colors) ++count;
		//if (mesh->GetElementTangentCount() > 0) ++count;
		return count;
	}



	void writeModelHeader()
	{
		FbxMesh* mesh = meshes[0].fbx;
		Model::FileHeader header;
		header.magic = 0x5f4c4d4f; // == '_LMO';
		header.version = (u32)Model::FileVersion::LATEST;
		write(header);
		u32 flags = (u32)Model::Flags::INDICES_16BIT;
		write(flags);


		i32 attribute_count = getAttributeCount(mesh);
		write(attribute_count);

		i32 pos_attr = 0;
		write(pos_attr);
		i32 nrm_attr = 1;
		write(nrm_attr);
		if (mesh->GetElementUVCount() > 0)
		{
			i32 uv0_attr = 8;
			write(uv0_attr);
		}
		if (isSkinned(mesh))
		{
			i32 indices_attr = 6;
			write(indices_attr);
			i32 weight_attr = 7;
			write(weight_attr);
		}
	}


	void makeTextureDirRelative()
	{
		if (texture_dir.empty()) return;

		Engine& engine = app.getWorldEditor()->getEngine();
		const char* base_path = engine.getDiskFileDevice()->getBasePath();
		char tmp[MAX_PATH_LENGTH];
		PathUtils::normalize(texture_dir, tmp, lengthOf(tmp));
		if (startsWith(tmp, base_path))
		{
			texture_dir = "/";
			texture_dir << tmp + stringLength(base_path);
		}
	}


	bool import()
	{
		if (!endsWith(output_dir.data, "/") && !endsWith(output_dir.data, "\\"))
		{
			output_dir << "/";
		}
		makeTextureDirRelative();
		if (!endsWith(texture_dir.data, "/") && !endsWith(texture_dir.data, "\\") && !texture_dir.empty())
		{
			texture_dir << "/";
		}

		writeModel();
		writeAnimations();
		writeMaterials();

		return true;
	}


	void writeModel()
	{
		auto cmpMeshes = [](const void* a, const void* b) -> int {
			auto a_mesh = static_cast<const ImportMesh*>(a);
			auto b_mesh = static_cast<const ImportMesh*>(b);
			return a_mesh->lod - b_mesh->lod;
		};

		bool import_any_mesh = false;
		for (const ImportMesh& m : meshes) if (m.import) import_any_mesh = true;
		if (!import_any_mesh) return;

		qsort(&meshes[0], meshes.size(), sizeof(meshes[0]), cmpMeshes);
		StaticString<MAX_PATH_LENGTH> out_path(output_dir, output_mesh_filename, ".msh");
		PlatformInterface::makePath(output_dir);
		if (!out_file.open(out_path, FS::Mode::CREATE_AND_WRITE, app.getWorldEditor()->getAllocator()))
		{
			g_log_error.log("FBX") << "Failed to create " << out_path;
			return;
		}

		writeModelHeader();
		writeMeshes();
		writeGeometry();
		writeSkeleton();
		writeLODs();
		out_file.close();
	}


	void clearSources()
	{
		for (FbxScene* scene : scenes) scene->Destroy();
		scenes.clear();
		meshes.clear();
		materials.clear();
		animations.clear();
		bones.clear();
	}


	~ImportFBXPlugin()
	{
		fbx_manager->Destroy();
	}


	void toggleOpened() { opened = !opened; }
	bool isOpened() const { return opened; }


	void onAnimationsGUI()
	{
		StaticString<30> label("Animations (");
		label << animations.size() << ")###Animations";
		if (!ImGui::CollapsingHeader(label)) return;

		/*ImGui::DragFloat("Time scale", &m_model.time_scale, 1.0f, 0, FLT_MAX, "%.5f");
		ImGui::DragFloat("Max position error", &m_model.position_error, 0, FLT_MAX);
		ImGui::DragFloat("Max rotation error", &m_model.rotation_error, 0, FLT_MAX);
*/
		ImGui::Indent();
		ImGui::Columns(3);

		ImGui::Text("Name");
		ImGui::NextColumn();
		ImGui::Text("Import");
		ImGui::NextColumn();
		ImGui::Text("Root motion bone");
		ImGui::NextColumn();
		ImGui::Separator();

		ImGui::PushID("anims");
		for (int i = 0; i < animations.size(); ++i)
		{
			ImportAnimation& animation = animations[i];
			ImGui::PushID(i);
			ImGui::InputText("", animation.output_filename.data, lengthOf(animation.output_filename.data));
			ImGui::NextColumn();
			ImGui::Checkbox("", &animation.import);
			ImGui::NextColumn();
			/*auto getter = [](void* data, int idx, const char** out) -> bool {
				auto* animation = (ImportAnimation*)data;
				*out = animation->animation->mChannels[idx]->mNodeName.C_Str();
				return true;
			};
			ImGui::Combo("##rb", &animation.root_motion_bone_idx, getter, &animation, animation.animation->mNumChannels);*/
			ImGui::NextColumn();
			ImGui::PopID();
		}

		ImGui::PopID();
		ImGui::Columns();
		ImGui::Unindent();
	}


	static const char* getImportMeshName(const ImportMesh& mesh)
	{
		const char* name = mesh.fbx->GetName();
		FbxSurfaceMaterial* material = mesh.fbx_mat;
		if (name[0] == '\0') name = mesh.fbx->GetNode()->GetName();
		if (name[0] == '\0' && material) name = material->GetName();
		return name;
	}


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
			const char* name = getImportMeshName(mesh);
			ImGui::Text("%s", name);
			ImGui::NextColumn();

			FbxSurfaceMaterial* material = mesh.fbx_mat;
			ImGui::Text("%s", material ? material->GetName() : "N/A");
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
				onAnimationsGUI();

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
				ImGui::InputText("Texture directory", texture_dir.data, sizeof(texture_dir));
				ImGui::SameLine();
				if (ImGui::Button("...###browsetexturedir"))
				{
					if (PlatformInterface::getOpenDirectory(texture_dir.data, sizeof(texture_dir), last_dir))
					{
						last_dir = texture_dir;
					}
				}

				if (ImGui::Button("Convert")) import();
			}
		}
		ImGui::EndDock();
	}


	const char* getName() const override { return "import_fbx"; }

	enum class Orientation
	{
		Y_UP,
		Z_UP,
		Z_MINUS_UP,
		X_MINUS_UP
	};

	StudioApp& app;
	bool opened = false;
	FbxManager* fbx_manager = nullptr;
	Array<ImportMaterial> materials;
	Array<ImportMesh> meshes;
	Array<ImportAnimation> animations;
	Array<FbxNode*> bones;
	Array<FbxScene*> scenes;
	StaticString<MAX_PATH_LENGTH> output_dir;
	StaticString<MAX_PATH_LENGTH> texture_dir;
	StaticString<MAX_PATH_LENGTH> last_dir;
	StaticString<MAX_PATH_LENGTH> output_mesh_filename;
	float lods_distances[4] = {-10, -100, -1000, -10000};
	FS::OsFile out_file;
	float mesh_scale = 1.0f;
	bool to_dds = false;
	bool center_mesh = false;
	Orientation orientation = Orientation::Y_UP;

};


LUMIX_STUDIO_ENTRY(lumixengine_fbx)
{
	auto& editor = *app.getWorldEditor();
	auto* plugin = LUMIX_NEW(editor.getAllocator(), ImportFBXPlugin)(app);
	app.addPlugin(*plugin);
}


} // namespace Lumix

