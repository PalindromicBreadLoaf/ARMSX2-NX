#version 460

// ShaderConvert ubershader for StretchRect. Ported from Metal/convert.metal.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D samp0;

layout (std140, binding = 0) uniform cb
{
	uint variant;
	uint bilinear;
};

// Subset of ShaderConvert handled here
// This must match GSDevice.h enum ordering
const uint DEPTH_COPY            = 1u;
const uint COLCLIP_INIT          = 7u;
const uint COLCLIP_RESOLVE       = 8u;
const uint RTA_CORRECTION        = 9u;
const uint RTA_DECORRECTION      = 10u;
const uint TRANSPARENCY_FILTER   = 11u;
const uint DEPTH32_TO_RGBA8      = 14u;
const uint DEPTH32_TO_RGB8       = 15u;
const uint DEPTH16_TO_RGB5A1     = 16u;
const uint RGBA8_TO_DEPTH32      = 17u;
const uint RGBA8_TO_DEPTH24      = 18u;
const uint RGBA8_TO_DEPTH16      = 19u;
const uint RGB5A1_TO_DEPTH16     = 20u;
const uint DEPTH32_TO_DEPTH24    = 21u;

vec4 depth32_to_rgba8(float value)
{
	uint val = uint(value * exp2(32.0));
	return vec4(uvec4(val, val >> 8u, val >> 16u, val >> 24u) & uvec4(0xFFu)) / 255.0;
}

vec4 depth16_to_rgba8(float value)
{
	uint val = uint(value * exp2(32.0));
	return vec4(uvec4(val << 3u, val >> 2u, val >> 7u, val >> 8u) & uvec4(0xf8u, 0xf8u, 0xf8u, 0x80u)) / 255.0;
}

float rgba8_to_depth32(vec4 unorm)
{
	uvec4 c = uvec4(unorm * 255.5);
	uint val = c.r | (c.g << 8u) | (c.b << 16u) | (c.a << 24u);
	return float(val) * exp2(-32.0);
}

float rgba8_to_depth24(vec4 unorm)
{
	uvec3 c = uvec3(unorm.rgb * 255.5);
	uint val = c.r | (c.g << 8u) | (c.b << 16u);
	return float(val) * exp2(-32.0);
}

float rgba8_to_depth16(vec4 unorm)
{
	uvec2 c = uvec2(unorm.rg * 255.5);
	uint val = c.r | (c.g << 8u);
	return float(val) * exp2(-32.0);
}

float rgb5a1_to_depth16(vec4 unorm)
{
	uvec4 cu = uvec4(unorm * 255.5);
	uint o = (cu.x >> 3u) | ((cu.y << 2u) & 0x03e0u) | ((cu.z << 7u) & 0x7c00u) | ((cu.w << 8u) & 0x8000u);
	return float(o) * exp2(-32.0);
}

float rgba_texel_to_depth(vec4 c, uint v)
{
	if (v == RGBA8_TO_DEPTH32)
		return rgba8_to_depth32(c);
	if (v == RGBA8_TO_DEPTH24)
		return rgba8_to_depth24(c);
	if (v == RGBA8_TO_DEPTH16)
		return rgba8_to_depth16(c);
	return rgb5a1_to_depth16(c);
}

// Manual bilinear done after the rgba->depth conversion
float biln_depth(uint v)
{
	ivec2 dim = textureSize(samp0, 0);
	vec2 tlf = vTexCoord * vec2(dim) - 0.5;
	ivec2 tl = ivec2(floor(tlf));
	ivec2 lo = clamp(tl, ivec2(0), dim - 1);
	ivec2 hi = clamp(tl + 1, ivec2(0), dim - 1);
	vec2 mv = fract(tlf);
	float dTL = rgba_texel_to_depth(texelFetch(samp0, ivec2(lo.x, lo.y), 0), v);
	float dTR = rgba_texel_to_depth(texelFetch(samp0, ivec2(hi.x, lo.y), 0), v);
	float dBL = rgba_texel_to_depth(texelFetch(samp0, ivec2(lo.x, hi.y), 0), v);
	float dBR = rgba_texel_to_depth(texelFetch(samp0, ivec2(hi.x, hi.y), 0), v);
	return mix(mix(dTL, dTR, mv.x), mix(dBL, dBR, mv.x), mv.y);
}

void main()
{
	if (variant == COLCLIP_INIT)
	{
		vec4 c = texture(samp0, vTexCoord);
		oColor = vec4(round(c.rgb * 255.0) / 65535.0, c.a);
	}
	else if (variant == COLCLIP_RESOLVE)
	{
		vec4 c = texture(samp0, vTexCoord);
		oColor = vec4(vec3(uvec3(c.rgb * 65535.5) & 255u) / 255.0, c.a);
	}
	else if (variant == RTA_CORRECTION)
	{
		vec4 c = texture(samp0, vTexCoord);
		oColor = vec4(c.rgb, c.a / (128.25 / 255.0));
	}
	else if (variant == RTA_DECORRECTION)
	{
		vec4 c = texture(samp0, vTexCoord);
		oColor = vec4(c.rgb, c.a * (128.25 / 255.0));
	}
	else if (variant == TRANSPARENCY_FILTER)
	{
		vec4 c = texture(samp0, vTexCoord);
		oColor = vec4(c.rgb, 1.0);
	}
	else if (variant == DEPTH32_TO_RGBA8 || variant == DEPTH32_TO_RGB8)
	{
		oColor = depth32_to_rgba8(texture(samp0, vTexCoord).r);
	}
	else if (variant == DEPTH16_TO_RGB5A1)
	{
		oColor = depth16_to_rgba8(texture(samp0, vTexCoord).r);
	}
	else if (variant == DEPTH_COPY)
	{
		gl_FragDepth = texture(samp0, vTexCoord).r;
		oColor = vec4(gl_FragDepth);
	}
	else if (variant == DEPTH32_TO_DEPTH24)
	{
		uint val = uint(texture(samp0, vTexCoord).r * exp2(32.0)) & 0xFFFFFFu;
		gl_FragDepth = float(val) * exp2(-32.0);
		oColor = vec4(gl_FragDepth);
	}
	else if (variant >= RGBA8_TO_DEPTH32 && variant <= RGB5A1_TO_DEPTH16)
	{
		gl_FragDepth = bilinear != 0u ? biln_depth(variant) : rgba_texel_to_depth(texture(samp0, vTexCoord), variant);
		oColor = vec4(gl_FragDepth);
	}
	else
	{
		oColor = texture(samp0, vTexCoord);
	}
}
