#include <fbxsdk.h>
#include <string>
#include <vector>
#include <cassert>


typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;


static u32 crc32Table[256] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d };


u32 crc32(const void* data, int length)
{
	const u8* c = static_cast<const u8*>(data);
	u32 crc = 0xffffFFFF;
	int len = length;
	while (len)
	{
		crc = (crc >> 8) ^ crc32Table[crc & 0xFF ^ *c];
		--len;
		++c;
	}
	return ~crc;
}

struct Transform
{
	float tx, ty, tz;
	float rx, ry, rz, rw;
};

#pragma pack(1)
struct FileHeader
{
	u32 magic;
	u32 version;
};
#pragma pack()

enum class FileVersion : u32
{
	FIRST,
	WITH_FLAGS,
	SINGLE_VERTEX_DECL,

	LATEST // keep this last
};

enum class Flags : u32
{
	INDICES_16BIT = 1 << 0
};

struct LOD
{
	int from_mesh;
	int to_mesh;

	float distance;
};

struct Bone
{
	std::string name;
	std::string parent;
	Transform transform;
	Transform inv_bind_transform;
	int parent_idx;
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


FILE* fp = NULL;
template <typename T>
void write(const T& obj)
{
	fwrite(&obj, sizeof(obj), 1, fp);
}

void write(const void* ptr, size_t size)
{
	fwrite(ptr, size, 1, fp);
}

void writeString(const char* str)
{
	fwrite(str, strlen(str), 1, fp);
}

void writeModelHeader()
{
	FileHeader header;
	header.magic = 0x5f4c4d4f; // == '_LMO';
	header.version = (u32)FileVersion::LATEST;
	write(header);
	u32 flags = (u32)Flags::INDICES_16BIT;
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

static u32 packF4u(const FbxVector4& vec)
{
	const u8 xx = u8(vec.mData[0] * 127.0f + 128.0f);
	const u8 yy = u8(vec.mData[1] * 127.0f + 128.0f);
	const u8 zz = u8(vec.mData[2] * 127.0f + 128.0f);
	const u8 ww = u8(0);
	return packuint32(xx, yy, zz, ww);
}

template <typename T>
static int indexOf(const T*const * array, const T* obj)
{
	int i = 0;
	const T*const * iter = array;
	while (*iter != obj) {
		++i;
		++iter;
	}
	return i;
}


void writeGeometry(FbxMesh** meshes, int count, FbxNode** bones)
{
	std::vector<Vertex> vertices;
	i32 indices_count = 0;

	for (int i = 0; i < count; ++i)
	{
		FbxMesh* mesh = meshes[i];
		assert(mesh->IsTriangleMesh());
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
		std::vector<Skin> skinning;
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
				FbxVector4 cp = mesh->GetControlPointAt(vertex_index);
				Vertex v;
				FbxAMatrix transform_matrix;
				FbxAMatrix geometry_matrix(
					mesh->GetNode()->GetGeometricTranslation(FbxNode::eSourcePivot),
					mesh->GetNode()->GetGeometricRotation(FbxNode::eSourcePivot),
					mesh->GetNode()->GetGeometricScaling(FbxNode::eSourcePivot));
				cluster->GetTransformMatrix(transform_matrix);
				transform_matrix *= geometry_matrix;
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
				vertices.push_back(v);
			}
		}
	}
	i32 vertices_size = (i32)(sizeof(vertices[0]) * vertices.size());
	write(vertices_size);
	write(&vertices[0], vertices_size);
}


FbxAMatrix getBindPoseMatrix(FbxMesh* mesh, FbxNode* node)
{
	if (!mesh) return FbxAMatrix();

	const FbxAMatrix geometry_matrix(
		mesh->GetNode()->GetGeometricTranslation(FbxNode::eSourcePivot),
		mesh->GetNode()->GetGeometricRotation(FbxNode::eSourcePivot),
		mesh->GetNode()->GetGeometricScaling(FbxNode::eSourcePivot));

	FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
	int deformer_count = mesh->GetDeformerCount();
	assert(deformer_count == 1);
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
	assert(scale > 0.99 && scale < 1.01);

	const FbxAMatrix bind_pose = inverse_bind_pose.Inverse();

	return bind_pose;
}


FbxMesh* getAnyMeshFromBone(FbxNode* node, FbxMesh** meshes, int mesh_count)
{
	for (int i = 0; i < mesh_count; ++i)
	{
		FbxMesh* mesh = meshes[i];
		FbxDeformer* deformer = mesh->GetDeformer(0, FbxDeformer::EDeformerType::eSkin);
		auto* skin = static_cast<FbxSkin*>(deformer);
		int cluster_count =  skin->GetClusterCount();
		for (int j = 0; j < cluster_count; ++j)
		{
			FbxCluster* cluster = skin->GetCluster(j);
			if (cluster->GetLink() == node) return mesh;
		}
	}

	return nullptr;
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


void gatherMaterials(FbxNode* node, std::vector<FbxSurfaceMaterial*>& materials)
{
	for (int i = 0; i < node->GetMaterialCount(); ++i)
	{
		materials.push_back(node->GetMaterial(i));
	}

	for (int i = 0; i < node->GetChildCount(); ++i)
	{
		gatherMaterials(node->GetChild(i), materials);
	}
}


void gatherBones(FbxNode* node, std::vector<FbxNode*>& bones)
{
	bool is_bone = node->GetNodeAttribute() && node->GetNodeAttribute()->GetAttributeType() == FbxNodeAttribute::EType::eSkeleton;
	// TODO use is_bone
	bones.push_back(node);

	int count = 1;
	for (int i = 0; i < node->GetChildCount(); ++i)
	{
		gatherBones(node->GetChild(i), bones);
	}
}


void gatherMeshes(FbxNode* node, std::vector<FbxMesh*>& meshes)
{
	FbxMesh* mesh = node->GetMesh();
	if (mesh) meshes.push_back(mesh);

	for (int i = 0; i < node->GetChildCount(); ++i)
	{
		gatherMeshes(node->GetChild(i), meshes);
	}
}


void getBasename(char (&out)[_MAX_PATH], const char* in)
{
	const char* tmp = strrchr(in, '/');
	if(!tmp) tmp = strrchr(in, '\\');
	if (!tmp) tmp = in;
	else tmp = tmp + 1;

	strcpy_s(out, tmp);
}


void writeMaterials(const char* dir, FbxSurfaceMaterial** materials, int count)
{
	for (int i = 0; i < count; ++i)
	{
		FbxSurfaceMaterial* material = materials[i];

		char path[_MAX_PATH];
		strcpy_s(path, dir);
		strcat_s(path, material->GetName());
		strcat_s(path, ".mat");
		fopen_s(&fp, path, "wb");

		writeString("{\n\t\"shader\" : \"pipelines/rigid/rigid.shd\"");

		FbxProperty diffuse = material->FindProperty(FbxSurfaceMaterial::sDiffuse);
		FbxFileTexture* texture = diffuse.GetSrcObject<FbxFileTexture>();

		if (texture)
		{
			writeString(",\n\t\"texture\" : { \"source\" : \"");
			char filename[_MAX_PATH];
			getBasename(filename, texture->GetFileName());
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
			getBasename(filename, texture->GetFileName());
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
		else if(material->GetClassId().Is(FbxSurfaceLambert::ClassId))
		{
			FbxSurfaceLambert* lambert = (FbxSurfaceLambert*)material;

		}

		writeString("}");

		fclose(fp);
	}
}


struct AnimHeader
{
	u32 magic = 0x5f4c4146; // '_LAF';
	u32 version = 3;
	u32 fps;
};


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
		if (take_info) {
			time_spawn = take_info->mLocalTimeSpan;
		}
		else {
			scene->GetGlobalSettings().GetTimelineDefaultTimeSpan(time_spawn);
		}

		FbxTime::EMode mode = scene->GetGlobalSettings().GetTimeMode();
		float scene_frame_rate =
			static_cast<float>((mode == FbxTime::eCustom)
				? scene->GetGlobalSettings().GetCustomFrameRate()
				: FbxTime::GetFrameRate(mode));

		float sampling_period = 1.0f / scene_frame_rate;

		float start = (float)(time_spawn.GetStart().GetSecondDouble());
		float end = (float)(time_spawn.GetStop().GetSecondDouble());

		float duration = end > start ? end - start : 1.0f;

		fopen_s(&fp, "C:/projects/hunter/models/test/out.ani", "wb");
		AnimHeader header;
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

			FbxNode* parent = bone;
			while (parent)
			{
				const char* pname = parent->GetName();
				auto ml = parent->EvaluateLocalTransform();
				auto mg = parent->EvaluateGlobalTransform();


				parent = parent->GetParent();
			}

			for (u16 f = 0; f < frames; ++f) write(f);
			for (u16 f = 0; f < frames; ++f)
			{
				float t = f * sampling_period;
				FbxAMatrix mtx = eval->GetNodeLocalTransform(bone, FbxTimeSeconds(t));
				auto scale = mtx.GetS().mData[0];
				assert(scale > 0.99f && scale < 1.01f);
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
		fclose(fp);
	}
}


int main(int argc, char** argv) {

	#define SRC_FBX 1
	#if SRC_FBX == 0
		const char* lFilename = "char+wep.fbx";
	#elif SRC_FBX == 1
		const char* lFilename = "shot.fbx";
	#elif SRC_FBX == 2
		const char* lFilename = "char.fbx";
	#else
		const char* lFilename = "wep.fbx";
	#endif

	FbxManager* lSdkManager = FbxManager::Create();
	FbxIOSettings *ios = FbxIOSettings::Create(lSdkManager, IOSROOT);
	lSdkManager->SetIOSettings(ios);

	FbxImporter* lImporter = FbxImporter::Create(lSdkManager, "");

	if (!lImporter->Initialize(lFilename, -1, lSdkManager->GetIOSettings())) {
		printf("Call to FbxImporter::Initialize() failed.\n");
		printf("Error returned: %s\n\n", lImporter->GetStatus().GetErrorString());
		exit(-1);
	}

	FbxScene* lScene = FbxScene::Create(lSdkManager, "myScene");

	lImporter->Import(lScene);
	lImporter->Destroy();

	FbxNode* lRootNode = lScene->GetRootNode();
	std::vector<FbxMesh*> meshes;
	gatherMeshes(lRootNode, meshes);
	std::vector<FbxSurfaceMaterial*> materials;
	gatherMaterials(lRootNode, materials);
	std::vector<FbxNode*> bones;
	gatherBones(lRootNode, bones);


	fopen_s(&fp, "C:/projects/hunter/models/test/out.msh", "wb");

	writeModelHeader();
	writeMeshes(&meshes[0], (int)meshes.size());
	writeGeometry(&meshes[0], (int)meshes.size(), &bones[0]);
	writeSkeleton(&bones[0], (int)bones.size(), &meshes[0], (int)meshes.size());
	writeLODs((int)meshes.size());
	fclose(fp);

	writeAnimations(lScene, &bones[0], (int)bones.size());

	//writeMaterials("C:/projects/hunter/models/test/" ,&materials[0], (int)materials.size());

	lSdkManager->Destroy();
	return 0;
}