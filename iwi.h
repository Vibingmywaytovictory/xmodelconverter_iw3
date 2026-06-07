#pragma once

#include "types.h"
#include "dxt.h"

struct IWI
{
	enum class USAGE_TYPE
	{
		INVALID = -1,
		COLOR,
		DEFAULT,
		SKYBOX = 0x5,
		DECAL_BEG = 9,
		DECAL_END = 19
	};

	enum class FORMAT_TYPE
	{
		INVALID = -1,
		ARGB32,
		RGB24,
		GA16,
		A8,
		DXT1 = 0xb,
		DXT3,
		DXT5
	};

	unsigned short width, height;
	USAGE_TYPE usage;
	FORMAT_TYPE format;

	u8* texture_data_ptr;
	size_t texture_data_sz;

	IWI() : usage(USAGE_TYPE::INVALID), format(FORMAT_TYPE::INVALID), width(0), height(0), texture_data_ptr(0), texture_data_sz(0) {}

	bool read_from_memory(BinaryReader &rd)
	{
		u8 magic[3];
		for (int i = 0; i < 3; i++)
			magic[i] = rd.read<u8>();
		u8 version = rd.read<u8>();

		// IWi5 = CoD4 (IW3); IWi6 = CoD:WaW / MW2 / MW3 / Ghosts / BO2 etc.
		// The fields read below (format, usage, dimensions, mip-offset table)
		// share the same layout across both versions, so accept either one.
		if (magic[0] != 'I' || magic[1] != 'W' || magic[2] != 'i' || (version != 5 && version != 6))
		{
			rd.set_error_message("unsupported IWI file '%c%c%c%d'\n", magic[0], magic[1], magic[2], version);
			return false;
		}

		format = (FORMAT_TYPE)rd.read<u8>();
		usage = (USAGE_TYPE)rd.read<u8>();

		width = rd.read<u16>();
		height = rd.read<u16>();

		rd.skip(2); // depth

		u32 filesize = rd.read<u32>();

		// The mip-offset table that follows is unreliable for images stored with
		// a single mip level (every entry equals the file size, e.g. lens
		// textures with the no-mipmap flag). The full-resolution mip is always
		// the last block in the file, so derive its size from the dimensions and
		// take the trailing bytes instead of trusting the table.
		size_t blockbytes = (format == FORMAT_TYPE::DXT1) ? 8 : 16;
		size_t blocksw = (width + 3) / 4; if (blocksw == 0) blocksw = 1;
		size_t blocksh = (height + 3) / 4; if (blocksh == 0) blocksh = 1;
		texture_data_sz = blocksw * blocksh * blockbytes;
		texture_data_ptr = rd.buffer() + (filesize - texture_data_sz);
		//printf("width=%d,height=%d,format=%d,usage=%d,texture_data_sz=%d\n", width, height, format, usage, texture_data_sz);
		//getchar();
		return true;
	}

	bool build_dds(std::vector<u8> &v) const
	{
		v.resize(sizeof(DDS_header));
		
		DDS_header* hdr = (DDS_header*)v.data();

		memset(hdr, 0, sizeof(DDS_header));
		hdr->dwMagic = ('D' << 0) | ('D' << 8) | ('S' << 16) | (' ' << 24);
		hdr->dwSize = 124;
		hdr->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
		hdr->dwWidth = width;
		hdr->dwHeight = height;
		hdr->dwPitchOrLinearSize = texture_data_sz;
		hdr->sPixelFormat.dwSize = 32;
		hdr->sPixelFormat.dwFlags = DDPF_FOURCC;
		if (format == FORMAT_TYPE::DXT1)
		{
			hdr->sPixelFormat.dwFourCC = ('D' << 0) | ('X' << 8) | ('T' << 16) | ('1' << 24);
		}
		else if (format == FORMAT_TYPE::DXT3)
		{
			hdr->sPixelFormat.dwFourCC = ('D' << 0) | ('X' << 8) | ('T' << 16) | ('3' << 24);
		}
		else
		{
			hdr->sPixelFormat.dwFourCC = ('D' << 0) | ('X' << 8) | ('T' << 16) | ('5' << 24);
		}
		hdr->sCaps.dwCaps1 = DDSCAPS_TEXTURE;
		v.insert(v.end(), texture_data_ptr, texture_data_ptr + texture_data_sz);
		return true;
	}
};