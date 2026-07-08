#version 460

// ShaderConvert::RGBA_TO_8I/RGB5A1_TO_8I for ConvertToIndexedTexture. Ported from vulkan/convert.glsl.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D samp0;

layout (std140, binding = 0) uniform cb
{
	uint SBW;
	uint DBW;
	uint PSM;
	uint variant; // 0 = RGBA_TO_8I, 1 = RGB5A1_TO_8I
	float ScaleFactor;
	vec3 pad;
};

void convert_rgba_8i()
{
	// Convert a RGBA texture into a 8 bits packed texture
	// Input 8x2 RGBA pixels
	// Outputs 16x4 Index pixels
	uvec2 pos = uvec2(gl_FragCoord.xy);

	// Collapse separate rgba areas into their base pixel
	uvec2 block = (pos & ~uvec2(15u, 3u)) >> 1;
	uvec2 subblock = pos & uvec2(7u, 1u);
	uvec2 coord = block | subblock;

	// Compensate for differing page pitch
	uvec2 block_xy = coord / uvec2(64u, 32u);
	uint block_num = (block_xy.y * (DBW / 128u)) + block_xy.x;
	uvec2 block_offset = uvec2((block_num % (SBW / 64u)) * 64u, (block_num / (SBW / 64u)) * 32u);
	coord = (coord % uvec2(64u, 32u)) + block_offset;

	// Apply offset to cols 1 and 2
	uint is_col23 = pos.y & 4u;
	uint is_col13 = pos.y & 2u;
	uint is_col12 = is_col23 ^ (is_col13 << 1);
	coord.x ^= is_col12; // If cols 1 or 2, flip bit 3 of x

	if (floor(ScaleFactor) != ScaleFactor)
		coord = uvec2(vec2(coord) * ScaleFactor);
	else
		coord *= uvec2(ScaleFactor);

	vec4 pixel = texelFetch(samp0, ivec2(coord), 0);
	vec2  sel0 = (pos.y & 2u) == 0u ? pixel.rb : pixel.ga;
	float sel1 = (pos.x & 8u) == 0u ? sel0.x : sel0.y;
	oColor = vec4(sel1);
}

void convert_rgb5a1_8i()
{
	// Convert a RGB5A1 texture into a 8 bits packed texture
	// Input 16x2 RGB5A1 pixels
	// Output 16x4 Index pixels

	uvec2 pos = uvec2(gl_FragCoord.xy);

	uvec2 column = (pos & ~uvec2(0u, 3u)) / uvec2(1, 2);
	uvec2 subcolumn = (pos & uvec2(0u, 1u));
	column.x -= (column.x / 128) * 64;
	column.y += (column.y / 32) * 32;

	// Deal with swizzling
	if ((PSM & 0x8u) != 0u) // PSMCT16S
	{
		if ((pos.x & 32u) != 0u)
		{
			column.y += 32u;
			column.x &= ~32u;
		}

		if ((pos.x & 64u) != 0u)
		{
			column.x -= 32u;
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((PSM & 0x30u) != 0u) // Untested but hopefully ok
		{
			column.x ^= 32u;
			column.y ^= 16u;
		}
	}
	else // PSMCT16
	{
		if ((pos.y & 32u) != 0u)
		{
			column.y -= 16u;
			column.x += 32u;
		}

		if ((pos.x & 96u) != 0u)
		{
			uint multi = (pos.x & 96u) / 32u;
			column.y += 16u * multi;
			column.x -= (pos.x & 96u);
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((PSM & 0x30u) != 0u) // Also untested. I love not testing things
		{
			column.x ^= 32u;
			column.y ^= 32u;
		}
	}
	uvec2 coord = column | subcolumn;

	uvec2 block_xy = coord / uvec2(64u, 64u);
	uint block_num = (block_xy.y * (DBW / 128u)) + block_xy.x;
	uvec2 block_offset = uvec2((block_num % (SBW / 64u)) * 64u, (block_num / (SBW / 64u)) * 64u);
	coord = (coord % uvec2(64u, 64u)) + block_offset;

	uint is_col23 = pos.y & 4u;
	uint is_col13 = pos.y & 2u;
	uint is_col12 = is_col23 ^ (is_col13 << 1);
	coord.x ^= is_col12; // If cols 1 or 2, flip bit 3 of x

	if (floor(ScaleFactor) != ScaleFactor)
		coord = uvec2(vec2(coord) * ScaleFactor);
	else
		coord *= uvec2(ScaleFactor);

	vec4 pixel = texelFetch(samp0, ivec2(coord), 0);
	uvec4 denorm_c = uvec4(pixel * 255.5f);
	if ((pos.y & 2u) == 0u)
	{
		uint red = (denorm_c.r >> 3) & 0x1Fu;
		uint green = (denorm_c.g >> 3) & 0x1Fu;
		float sel0 = float(((green << 5) | red) & 0xFFu) / 255.0f;

		oColor = vec4(sel0);
	}
	else
	{
		uint green = (denorm_c.g >> 3) & 0x1Fu;
		uint blue = (denorm_c.b >> 3) & 0x1Fu;
		uint alpha = denorm_c.a & 0x80u;
		float sel0 = float((alpha | (blue << 2) | (green >> 3)) & 0xFFu) / 255.0f;

		oColor = vec4(sel0);
	}
}

void main()
{
	if (variant == 0u)
		convert_rgba_8i();
	else
		convert_rgb5a1_8i();
}
