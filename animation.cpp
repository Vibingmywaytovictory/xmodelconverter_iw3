#include "types.h"
#include "util.h"
#include <glm/gtc/quaternion.hpp>
#include <iterator>

// Sample a sorted keyframe track at an arbitrary frame, interpolating between
// the surrounding keys (slerp for rotation, lerp for translation) and clamping
// to the first/last key outside the keyed range. The CoD mod tools store sparse
// keyframes and interpolate on export, so we do the same to reproduce them.
static glm::quat sample_rotation(const std::map<int, glm::quat>& keys, int frame)
{
	auto hi = keys.lower_bound(frame);
	if (hi == keys.end())
		return keys.rbegin()->second;
	if (hi->first == frame || hi == keys.begin())
		return hi->second;
	auto lo = std::prev(hi);
	float t = (float)(frame - lo->first) / (float)(hi->first - lo->first);
	return glm::slerp(lo->second, hi->second, t);
}

static vec3 sample_translation(const std::map<int, vec3>& keys, int frame)
{
	auto hi = keys.lower_bound(frame);
	if (hi == keys.end())
		return keys.rbegin()->second;
	if (hi->first == frame || hi == keys.begin())
		return hi->second;
	auto lo = std::prev(hi);
	float t = (float)(frame - lo->first) / (float)(hi->first - lo->first);
	return lo->second * (1.f - t) + hi->second * t;
}

void XAnim::read_translations(const std::string& tag)
{
	u16 numtrans = m_reader->read<u16>();
	if (numtrans == 0)
		return;
	// A count larger than the frame count means the stream has desynced
	// (unsupported variant); bail before doing any huge reads.
	if (numtrans > m_numframes + 1)
	{
		m_valid = false;
		return;
	}
	vec3 v;
	if (numtrans == 1)
	{
		v = m_reader->read<vec3>();
		//printf("\tnumtrans 1 for '%s' %f,%f,%f\n", tag.c_str(), v.x, v.y, v.z);
		m_animframes[0].trans[tag] = v;
		return;
	}

	std::vector<int> frames;

	if (numtrans == m_numframes)
	{
		for (int i = 0; i < numtrans; ++i)
			frames.push_back(i);
	}
	else if (m_numframes > 0xff)
	{
		for (int i = 0; i < numtrans; ++i)
			frames.push_back(m_reader->read<u16>());
	}
	else
	{
		for (int i = 0; i < numtrans; ++i)
			frames.push_back(m_reader->read<u8>());
	}

	if (m_version == 0x11)
	{
		// CoD4/MW (v17): translations are quantized. After the frame indices
		// come a flag byte, a vec3 minimum and a vec3 size (float), then one
		// sample per key. flag==1 -> 3x u8 (value = mins + q/255 * size);
		// flag==0 -> 3x u16 (value = mins + q/65535 * size), the higher-
		// precision variant used by larger anims.
		u8 flag = m_reader->read<u8>();
		vec3 mins = m_reader->read<vec3>();
		vec3 size = m_reader->read<vec3>();
		for (int i = 0; i < numtrans; ++i)
		{
			if (flag == 1)
			{
				u8 qx = m_reader->read<u8>();
				u8 qy = m_reader->read<u8>();
				u8 qz = m_reader->read<u8>();
				v.x = mins.x + (qx / 255.f) * size.x;
				v.y = mins.y + (qy / 255.f) * size.y;
				v.z = mins.z + (qz / 255.f) * size.z;
			}
			else
			{
				u16 qx = m_reader->read<u16>();
				u16 qy = m_reader->read<u16>();
				u16 qz = m_reader->read<u16>();
				v.x = mins.x + (qx / 65535.f) * size.x;
				v.y = mins.y + (qy / 65535.f) * size.y;
				v.z = mins.z + (qz / 65535.f) * size.z;
			}
			m_animframes[frames[i]].trans[tag] = v;
		}
	}
	else
	{
		for (int i = 0; i < numtrans; ++i)
		{
			v = m_reader->read<vec3>();
			//printf("trans frame %d -> %f,%f,%f\n", frames[i], v.x, v.y, v.z);
			m_animframes[frames[i]].trans[tag] = v;
		}
	}
}

// Validate (without consuming) that a v17 translation block at 'pos' is
// well-formed: a sane count, in-range ascending frame indices and a 0/1 flag.
// Used as a one-bone lookahead to tell whether a rotation is stored as the
// full 3x i16 form or the 2-byte single-axis form.
bool XAnim::peek_translation_valid(size_t pos)
{
	const std::vector<char>& buf = m_reader->m_buf;
	size_t n = buf.size();
	auto u16at = [&](size_t p) -> int { return (u8)buf[p] | ((u8)buf[p + 1] << 8); };
	if (pos + 2 > n)
		return false;
	int nt = u16at(pos);
	if (nt == 0)
		return true;
	if (nt > m_numframes + 1)
		return false;
	if (nt == 1)
		return pos + 2 + 12 <= n;
	size_t il = (nt == m_numframes) ? 0 : ((m_numframes > 0xff) ? (size_t)nt * 2 : (size_t)nt);
	if (pos + 2 + il + 1 > n)
		return false;
	if (il > 0)
	{
		int prev = -1;
		for (int k = 0; k < nt; ++k)
		{
			int f = (m_numframes > 0xff) ? u16at(pos + 2 + 2 * k) : (u8)buf[pos + 2 + k];
			if (f < prev || f > m_numframes)
				return false;
			prev = f;
		}
	}
	u8 flag = (u8)buf[pos + 2 + il];
	return flag == 0 || flag == 1;
}

void XAnim::read_rotations(const std::string& tag, bool flipquat, bool simplequat)
{
	u16 numrot = m_reader->read<u16>();
	//printf("numrot=%d\n", numrot);
	if (numrot == 0)
		return;
	// Sanity guard: more rotations than frames means the stream desynced.
	if (numrot > m_numframes + 1)
	{
		m_valid = false;
		return;
	}

	std::vector<int> frames;

	if (numrot == 1 || numrot == m_numframes)
	{
		for (int i = 0; i < numrot; ++i)
			frames.push_back(i);
	}
	else if (m_numframes > 0xff)
	{
		for (int i = 0; i < numrot; ++i)
			frames.push_back(m_reader->read<u16>());
	}
	else
	{
		for (int i = 0; i < numrot; ++i)
			frames.push_back(m_reader->read<u8>());
	}

	// v17: the simple-quat flag is only a *candidate*. Most flagged bones still
	// store full 3x i16 rotations; only single-axis ones use the 2-byte form.
	// Decide by checking whether reading full rotations would leave a valid
	// translation block - if not, this bone uses the 2-byte form.
	if (m_version == 0x11)
	{
		if (simplequat)
			simplequat = !peek_translation_valid(m_reader->m_pos + (size_t)numrot * 6);
	}

	for (int i = 0; i < numrot; ++i)
	{
		glm::quat q = m_reader->read_quat(flipquat, simplequat);
		//printf("frame %d '%s' -> q = %f,%f,%f,%f flip=%d,simple=%d\n", frames[i], tag.c_str(), q.x, q.y, q.z, q.w, flipquat, simplequat);
		if (!flipquat)
		{
			m_animframes[frames[i]].quats[tag] = q;
		}
	}
}

bool XAnim::read_xanim_file(BinaryReader &rd)
{
	m_reader = &rd;
	m_valid = true;
	m_version = rd.read<u16>();
	if (m_version != 0xe && m_version != 0x11)
		return rd.set_error_message("expected xanim version 0xe or 0x11, got %x\n", m_version);
	m_numframes = rd.read<u16>();
	m_numparts = rd.read<u16>();
	m_flags = rd.read<u8>();
	if (m_version == 0x11)
		rd.read<u8>(); // v17: extra byte after flags
	m_framerate = rd.read<u16>();
	if (m_version == 0x11)
		rd.read<u16>(); // v17: extra u16 after framerate

	bool looping = (m_flags & 0x1) == 0x1;
	bool delta = (m_flags & 0x2) == 0x2;
	m_frequency = ((float)m_framerate) / ((float)m_numframes);
	if (delta)
	{
		read_rotations("tag_origin", false, true);
		read_translations("tag_origin");
	}
	if (looping)
		++m_numframes;

	std::vector<std::string> partnames;

	// v14 uses ((numparts-1)>>3)+1 bytes per flag array; v17 uses numparts>>3
	// (anims with fewer than 8 parts carry no flag bytes at all).
	int boneflagssize = (m_version == 0x11) ? (m_numparts >> 3) : (((m_numparts - 1) >> 3) + 1);
	std::vector<u8> flipflags = rd.read_typed_buffer_to_vector<u8>(boneflagssize);
	std::vector<u8> simpleflags = rd.read_typed_buffer_to_vector<u8>(boneflagssize);
	for (int i = 0; i < m_numparts; ++i)
	{
		std::string partname;
		if (!rd.read_null_terminated_string(partname))
			break;
		partnames.push_back(partname);
	}
	for (int i = 0; i < m_numparts; ++i)
	{
		bool flipquat = false;
		bool simplequat = false;
		if (m_version == 0xe)
		{
			flipquat = ((1 << (i & 7)) & flipflags[i >> 3]) != 0;
			simplequat = ((1 << (i & 7)) & simpleflags[i >> 3]) != 0;
		}
		else
		{
			// v17: pass the simple flag as a candidate; read_rotations confirms
			// the actual 2-byte vs 6-byte form via a translation-block lookahead.
			if ((i >> 3) < (int)simpleflags.size())
				simplequat = ((1 << (i & 7)) & simpleflags[i >> 3]) != 0;
		}
		read_rotations(partnames[i], flipquat, simplequat);
		read_translations(partnames[i]);
		if (!m_valid)
			return rd.set_error_message("xanim stream desynced at part %d ('%s'); unsupported variant\n", i, partnames[i].c_str());
	}

	// Regroup the decoded keyframes per bone so we can sample every output
	// frame, interpolating between sparse keys the way the mod tools do.
	std::map<std::string, std::map<int, glm::quat>> rotkeys;
	std::map<std::string, std::map<int, vec3>> transkeys;
	for (auto& af : m_animframes)
	{
		for (auto& q : af.second.quats)
			rotkeys[q.first][af.first] = q.second;
		for (auto& t : af.second.trans)
			transkeys[t.first][af.first] = t.second;
	}

	for (int i = 0; i < m_numframes; ++i)
	{
		std::vector<Bone> refframe;
		refframe.resize(m_reference->parts.bones.size());
		int refboneindex = 0;
		for (auto& refbone : m_reference->parts.bones)
		{
			Bone xb = refbone; //start from the reference (bind) pose

			auto rit = rotkeys.find(refbone.name);
			if (rit != rotkeys.end() && !rit->second.empty())
				xb.transform.rotation = sample_rotation(rit->second, i);

			auto tit = transkeys.find(refbone.name);
			//ignore tag_origin translation so the animation plays on the spot
			if (tit != transkeys.end() && !tit->second.empty() && refbone.name != "tag_origin")
				xb.transform.translation = sample_translation(tit->second, i);

			refframe[refboneindex] = xb;
			++refboneindex;
		}
		m_refframes.push_back(refframe);
	}
	u8 notify_count = rd.read<u8>();
        for (int i = 0; i < notify_count; ++i)
        {
		//TODO: write this to xanim_export
                std::string notify_string;
                rd.read_null_terminated_string(notify_string);
                u16 notify_time_value = rd.read<u16>();
                printf("notify string: %s:%f, frame: %d\n", notify_string.c_str(), ((float)notify_time_value / ((float)m_numframes)), notify_time_value);
        }
	return true;
}

bool XAnim::export_file(const std::string& filename)
{
	FILE* fp = NULL;
	std::string fullfilename = filename + ".xanim_export";
	fopen_s(&fp, fullfilename.c_str(), "w");

	if (!fp)
		return false;

	fprintf(fp, "// This was file generated with https://github.com/riicchhaarrd/xmodelconverter\n");
	fprintf(fp, "ANIMATION\n");
	fprintf(fp, "VERSION 3\n");
	fprintf(fp, "\n");
	fprintf(fp, "NUMPARTS %d\n", m_reference->parts.bones.size());
	int refboneidx = 0;
	for (auto& it : m_reference->parts.bones)
	{
		fprintf(fp, "PART %d \"%s\"\n", refboneidx++, it.name.c_str());
	}
	fprintf(fp, "\n");
	fprintf(fp, "FRAMERATE %d\n", m_framerate);
	fprintf(fp, "NUMFRAMES %d\n", m_refframes.size());
	fprintf(fp, "\n");
	int frameno = 0;
	for (auto& refframe : m_refframes)
	{
		fprintf(fp, "FRAME %d\n", frameno++);
		refboneidx = 0;
		for (auto& xb : refframe)
		{
			//mat4 mat = refframe[refboneidx].bp;// getWorldMatrix(refframe, refboneidx);
			auto mat = util::get_world_transform(refframe, refboneidx).get_matrix();
			vec3 offset = util::get_translation_component_from_matrix(mat);
			fprintf(fp, "PART %d\n", refboneidx);
			fprintf(fp, "OFFSET %f, %f, %f\n", offset.x, offset.y, offset.z);
			fprintf(fp, "SCALE 1.000000, 1.000000, 1.000000\n");

			vec3 x, y, z;
			util::get_xyz_components_from_matrix(mat, x, y, z);
			fprintf(fp, "X %f, %f, %f\n", x.x, x.y, x.z);
			fprintf(fp, "Y %f, %f, %f\n", y.x, y.y, y.z);
			fprintf(fp, "Z %f, %f, %f\n", z.x, z.y, z.z);
			fprintf(fp, "\n");
			++refboneidx;
		}
	}
	fprintf(fp, "NOTETRACKS\n");
	refboneidx = 0;
	for (auto& it : m_reference->parts.bones)
	{
		fprintf(fp, "PART %d\n", refboneidx++);
		fprintf(fp, "NUMTRACKS 0\n");
		fprintf(fp, "\n");
	}
	fclose(fp);
	return true;
}
