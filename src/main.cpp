#include "animation/animation.h"
#include "editor/platform_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/fs/os_file.h"
#include "engine/iplugin.h"
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


	static FbxMesh* getAnyMeshFromBone(FbxNode* node, FbxMesh** meshes, int mesh_count)
	{
		for (int i = 0; i < mesh_count; ++i)
		{
			FbxMesh* mesh = meshes[i];
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


	static void gatherMaterials(FbxNode* node, Array<FbxSurfaceMaterial*>& materials)
	{
		for (int i = 0; i < node->GetMaterialCount(); ++i)
		{
			materials.push(node->GetMaterial(i));
		}

		for (int i = 0; i < node->GetChildCount(); ++i)
		{
			gatherMaterials(node->GetChild(i), materials);
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


	static void gatherMeshes(FbxNode* node, Array<FbxMesh*>& meshes)
	{
		FbxMesh* mesh = node->GetMesh();
		if (mesh) meshes.push(mesh);

		for (int i = 0; i < node->GetChildCount(); ++i)
		{
			gatherMeshes(node->GetChild(i), meshes);
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
	{
		Action* action = LUMIX_NEW(app.getWorldEditor()->getAllocator(), Action)("Import FBX", "import_fbx");
		action->func.bind<ImportFBXPlugin, &ImportFBXPlugin::toggleOpened>(this);
		action->is_selected.bind<ImportFBXPlugin, &ImportFBXPlugin::isOpened>(this);
		app.addWindowAction(action);

		fbx_manager = FbxManager::Create();
		FbxIOSettings* ios = FbxIOSettings::Create(fbx_manager, IOSROOT);
		fbx_manager->SetIOSettings(ios);
	}


	bool import(const char* filename)
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
			PathUtils::getBasename(output_filename.data, lengthOf(output_filename.data), filename);
		}
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


	void writeMaterials(FbxSurfaceMaterial** materials, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			FbxSurfaceMaterial* material = materials[i];

			StaticString<MAX_PATH_LENGTH> path(output_dir, material->GetName(), ".mat");
			IAllocator& allocator = app.getWorldEditor()->getAllocator();
			if (!out_file.open(path, FS::Mode::CREATE_AND_WRITE, allocator))
			{
				// TODO error message
				continue;
			}

			writeString("{\n\t\"shader\" : \"pipelines/rigid/rigid.shd\"");

			FbxProperty diffuse = material->FindProperty(FbxSurfaceMaterial::sDiffuse);
			FbxFileTexture* texture = diffuse.GetSrcObject<FbxFileTexture>();

			if (texture)
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"");
				char filename[_MAX_PATH];
				PathUtils::getBasename(filename, lengthOf(filename), texture->GetFileName());
				writeString(filename);
				writeString("\" } ");
			}
			else
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"\" }");
			}

			FbxProperty normal = material->FindProperty(FbxSurfaceMaterial::sNormalMap);
			texture = diffuse.GetSrcObject<FbxFileTexture>();

			if (texture)
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"");
				char filename[_MAX_PATH];
				PathUtils::getBasename(filename, lengthOf(filename), texture->GetFileName());
				writeString(filename);
				writeString("\" } ");
			}
			else
			{
				writeString(",\n\t\"texture\" : { \"source\" : \"\" }");
			}


			if (material->GetClassId().Is(FbxSurfacePhong::ClassId))
			{
				FbxSurfacePhong* phong = (FbxSurfacePhong*)material;
			}
			else if (material->GetClassId().Is(FbxSurfaceLambert::ClassId))
			{
				FbxSurfaceLambert* lambert = (FbxSurfaceLambert*)material;

			}

			writeString("}");

			out_file.close();
		}
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

				write(frames);

				for (u16 f = 0; f < frames; ++f) write(f);
				for (u16 f = 0; f < frames; ++f)
				{
					float t = f * sampling_period;
					FbxAMatrix mtx = eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t));
					auto scale = mtx.GetS().mData[0];
					// TODO check this in isValid function
					// assert(scale > 0.99f && scale < 1.01f);
					for (int i = 0; i < 3; ++i) write((float)mtx.GetT().mData[i]);
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


	void writeGeometry(FbxMesh** meshes, int count, FbxNode** bones)
	{
		IAllocator& allocator = app.getWorldEditor()->getAllocator();
		Array<Vertex> vertices(allocator);
		i32 indices_count = 0;

		for (int i = 0; i < count; ++i)
		{
			FbxMesh* mesh = meshes[i];
			indices_count += mesh->GetPolygonVertexCount();
		}
		write(indices_count);
	
		for (int k = 0; k < count; ++k)
		{
			FbxMesh* mesh = meshes[k];

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
					v.px = (float)cp.mData[0];
					v.py = (float)cp.mData[1];
					v.pz = (float)cp.mData[2];

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


	void writeMeshes(FbxMesh** meshes, int count)
	{
		i32 mesh_count = count;
		write(mesh_count);

		i32 attr_offset = 0;
		i32 indices_offset = 0;
		for (int i = 0; i < count; ++i)
		{
			FbxMesh* mesh = meshes[i];
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

			const char* name = "mesh_test_name";
			i32 name_len = (i32)strlen(name);
			write(name_len);
			write(name, strlen(name));
		}
	}


	void writeSkeleton(FbxNode** bones, int bone_count, FbxMesh** meshes, int mesh_count)
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

			FbxMesh* mesh = getAnyMeshFromBone(node, meshes, mesh_count);
			FbxAMatrix tr = getBindPoseMatrix(mesh, node);

			auto q = tr.GetQ();
			auto t = tr.GetT();
			for (int i = 0; i < 3; ++i) write((float)tr.GetT().mData[i]);
			for (double d : tr.GetQ().mData) write((float)d);
		}
	}


	void writeLODs(int count)
	{
		i32 lod_count = 1;
		write(lod_count);
		i32 to_mesh = count - 1;
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


	bool convert()
	{
		StaticString<MAX_PATH_LENGTH> out_path(output_dir, output_filename, ".msh");
		if (!out_file.open(out_path, FS::Mode::CREATE_AND_WRITE, app.getWorldEditor()->getAllocator()))
		{
			error_message << "Failed to create " << out_path;
			return false;
		}
		
		IAllocator& allocator = app.getWorldEditor()->getAllocator();
		Array<FbxMesh*> meshes(allocator);
		Array<FbxSurfaceMaterial*> materials(allocator);
		Array<FbxNode*> bones(allocator);
		for (FbxScene* scene : scenes)
		{
			FbxNode* root = scene->GetRootNode();
			gatherMeshes(root, meshes);
			gatherMaterials(root, materials);
			gatherBones(root, bones);
		}

		writeModelHeader();
		writeMeshes(&meshes[0], meshes.size());
		writeGeometry(&meshes[0], (int)meshes.size(), &bones[0]);
		writeSkeleton(&bones[0], (int)bones.size(), &meshes[0], (int)meshes.size());
		writeLODs((int)meshes.size());
		out_file.close();

		ASSERT(scenes.size() == 1); // only 1 scene supported ATM
		writeAnimations(scenes[0], &bones[0], bones.size());

		writeMaterials(&materials[0], materials.size());

		return true;
	}


	void clearScenes()
	{
		for (FbxScene* scene : scenes) scene->Destroy();
		scenes.clear();
	}


	~ImportFBXPlugin()
	{
		fbx_manager->Destroy();
	}


	void toggleOpened() { opened = !opened; }
	bool isOpened() const { return opened; }


	void onWindowGUI() override
	{
		if (ImGui::BeginDock("Import FBX", &opened))
		{
			if (ImGui::Button("Add source"))
			{
				char src_path[MAX_PATH_LENGTH];
				if (PlatformInterface::getOpenFilename(src_path, lengthOf(src_path), "All\0*.*\0", nullptr))
				{
					import(src_path);
				}
			}

			ImGui::InputText("Output directory", output_dir.data, sizeof(output_dir));
			ImGui::SameLine();
			if (ImGui::Button("...###browseoutput"))
			{
				if (PlatformInterface::getOpenDirectory(output_dir.data, sizeof(output_dir), last_dir))
				{
					last_dir = output_dir;
				}
			}
			ImGui::InputText("Output filename", output_filename.data, sizeof(output_filename));

			if (ImGui::Button("Convert")) convert();
		}
		ImGui::EndDock();
	}


	const char* getName() const override { return "import_fbx"; }


	StudioApp& app;
	bool opened = false;
	FbxManager* fbx_manager = nullptr;
	StaticString<1024> error_message;
	StaticString<MAX_PATH_LENGTH> output_dir;
	StaticString<MAX_PATH_LENGTH> last_dir;
	StaticString<MAX_PATH_LENGTH> output_filename;
	Array<FbxScene*> scenes;
	FS::OsFile out_file;
};


LUMIX_STUDIO_ENTRY(lumixengine_fbx)
{
	auto& editor = *app.getWorldEditor();
	auto* plugin = LUMIX_NEW(editor.getAllocator(), ImportFBXPlugin)(app);
	app.addPlugin(*plugin);
}


} // namespace Lumix

