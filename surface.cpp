#include "types.h"
#include "util.h"

bool XModelSurface::read_xmodelsurface_file(XModelParts &parts, BinaryReader &rd)
{
	this->clear();
	u16 version = rd.read<u16>();
	if (version != 0x14 && version != 0x19)
		return rd.set_error_message("expected xmodelsurface version 0x14 or 0x19, got %x\n", version);
	//printf("xmodelsurface version %d\n", version);

	//used for checking against xmodel numsurfs for file version conflict (e.g iwd/non-iwd)
	u16 numsurfs = rd.read<u16>();

	int idx = 0;
	for (int i = 0; i < numsurfs; ++i)
	{
		std::vector<Vertex> vertices;
		u8 tilemode = rd.read<u8>();
		u16 vertcount;
		u16 tricount;
		i16 boneoffset;
		if (version == 0x19)
		{
			// CoD4/MW (v25) surface header.
			//   tileMode(1) unk(2) vertCount(2) triCount(2) firstGroup(2) ...
			// RIGID surfaces repeat vertCount in 'firstGroup', then a default bone
			// index and a single 0 (13-byte header, fixed 60-byte verts bound to
			// one bone). SKINNED surfaces instead store a variable-length list of
			// (groupVertCount, bonesPerVert) pairs terminated by a (0,0) pair; the
			// group counts sum to vertCount, so the header length varies (e.g. 19
			// bytes for 2 groups, 23 bytes for 3).
			// NOTE: the previous code used a fixed header length and read the
			// rigid/skinned flag from the wrong offset, so it never detected
			// skinned surfaces and desynced (crash) on skinned models.
			rd.read<u16>();                 // unknown per-surface value (hash)
			vertcount = rd.read<u16>();
			tricount = rd.read<u16>();
			u16 firstgroup = rd.read<u16>();
			bool skinned = (firstgroup != vertcount);
			if (skinned)
			{
				// 'firstgroup' was the vertex count of the first weight group;
				// walk the remaining (count, bonesPerVert) pairs up to the (0,0)
				// terminator so we consume exactly this surface's header.
				rd.read<u16>();             // bonesPerVert of the first group
				for (;;)
				{
					u16 groupcount = rd.read<u16>();
					rd.read<u16>();         // bonesPerVert (or terminator's second half)
					if (groupcount == 0)
						break;
				}
				boneoffset = -1;            // triggers per-vertex skinning below
			}
			else
			{
				// remaining 2 shorts complete the 13-byte rigid header
				rd.read<u16>();             // default skin bone (geometry is model-space)
				rd.read<u16>();
				boneoffset = 0;             // rigid verts bind to root bone 0
			}
		}
		else
		{
			// CoD2 (v20)
			vertcount = rd.read<u16>();
			tricount = rd.read<u16>();
			boneoffset = rd.read<i16>();
			if (boneoffset == -1)
			{
				rd.read<u16>();
			}
		}

		for (int j = 0; j < vertcount; ++j)
		{
			Vertex vtx;
			vtx.numweights = 0;
			vec3 n = rd.read<vec3>();
			n *= -1.f;
			u32 color = rd.read<u32>();
			float u, v;
			u = rd.read<float>();
			v = rd.read<float>();
			vec3 binormal = rd.read<vec3>();
			vec3 tangent = rd.read<vec3>();

			u8 numweights = 0;
			u16 boneindex = boneoffset == -1 ? 0 : boneoffset;
			vec3 offset;

			vtx.normal = n;
			vtx.uv.x = u;
			vtx.uv.y = v;
			vtx.binormal = binormal;
			vtx.tangent = tangent;

			if (boneoffset == -1)
			{
				numweights = rd.read<u8>();
				boneindex = rd.read<u16>();
			}
			offset = rd.read<vec3>();
			vtx.numweights = numweights + 1;
			vtx.boneweights[0] = 1.f;
			vtx.boneindices[0] = boneindex;

			if (numweights > 0)
			{
				rd.read<u8>(); //idk weight?
				for (int k = 0; k < numweights; ++k)
				{
					u16 blendindex = rd.read<u16>();
					vec3 blendoffset = rd.read<vec3>();
					float blendweight = ((float)rd.read<u16>()) / (float)USHRT_MAX;
					vtx.boneweights[0] -= blendweight;
					vtx.boneweights[k + 1] = blendweight;
					vtx.boneindices[k + 1] = blendindex;
				}
			}
			auto transform = util::get_world_transform(parts.bones, boneindex);
			vtx.pos = glm::rotate(transform.rotation, offset) + transform.translation;
			vtx.normal = glm::rotate(transform.rotation, vtx.normal);
			vertices.push_back(vtx);
		}
		Mesh mesh;
		for (int i = 0; i < tricount; ++i)
		{
			u16 face[3];
			face[0] = rd.read<u16>();
			face[2] = rd.read<u16>();
			face[1] = rd.read<u16>();

			for (int j = 0; j < 3; ++j)
			{
				this->vertices.push_back(vertices.at(face[j]));
				mesh.indices.push_back(idx++);
			}
		}
		this->meshes.push_back(mesh);
	}

	return true;
}
