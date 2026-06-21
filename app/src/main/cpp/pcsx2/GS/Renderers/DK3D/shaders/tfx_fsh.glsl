#version 460

// Runtime-selected tfx fragment shader
// Mostly just a port of the Vulkan one with cbSel

#define FMT_32 0u
#define FMT_24 1u
#define FMT_16 2u

layout(std140, binding = 1) uniform cb1
{
	vec3 FogColor;
	float AREF;
	vec4 WH;
	vec2 TA;
	float MaxDepthPS;
	float Af;
	uvec4 FbMask;
	vec4 HalfTexel;
	vec4 MinMax;
	vec4 LODParams;
	vec4 STRange;
	ivec4 ChannelShuffle;
	vec2 TC_OffsetHack;
	vec2 STScale;
	mat4 DitherMatrix;
	float ScaledScaleFactor;
	float RcpScaleFactor;
};

layout(std140, binding = 0) uniform cbSel
{
	uint sel_fst;
	uint sel_tme;
	uint sel_tfx;
	uint sel_tcc;
	uint sel_atst;
	uint sel_afail;
	uint sel_fog;
	uint sel_aem;
	uint sel_aem_fmt;
	uint sel_pal_fmt;
	uint sel_ltf;
	uint sel_wms;
	uint sel_wmt;
	uint sel_dst_fmt;
	uint sel_fba;
	uint sel_iip;
	uint sel_region_rect;
	uint sel_adjs;
	uint sel_adjt;
	uint sel_tcoffsethack;
	uint sel_blend_a;
	uint sel_blend_b;
	uint sel_blend_c;
	uint sel_blend_d;
	uint sel_blend_mix;
	uint sel_blend_hw;
	uint sel_pabe;
	uint sel_fixed_one_a;
	uint sel_a_masked;
	uint sel_colclip;
	uint sel_colclip_hw;
	uint sel_rta_correction;
	uint sel_dither;
	uint sel_dither_adjust;
	uint sel_round_inv;
	uint sel_tex_is_fb;
	uint sel_channel;
	uint sel_shuffle;
	uint sel_shuffle_same;
	uint sel_read16src;
	uint sel_process_ba;
	uint sel_process_rg;
	uint sel_shuffle_across;
	uint sel_write_rg;
	uint sel_fbmask;
	uint sel_scanmsk;
	uint sel_date;
	uint sel_depth_fmt;
	uint sel_urban_chaos;
	uint sel_tales;
	uint sel_automatic_lod;
	uint sel_manual_lod;
};

layout(binding = 2) uniform sampler2D Texture;
layout(binding = 3) uniform sampler2D Palette;
// Bound only for feedback-loop draws
layout(binding = 4) uniform sampler2D RtSampler;
// Bound only for PrimID-tracking (DATE 3)
layout(binding = 5) uniform sampler2D PrimMinTexture;

layout(location = 0) in vec4 v_t;
layout(location = 1) in vec4 v_ti;
layout(location = 2) in vec4 v_c;
layout(location = 3) flat in vec4 v_cf;

layout(location = 0, index = 0) out vec4 o_col0;
layout(location = 0, index = 1) out vec4 o_col1;

vec4 sample_from_rt()
{
	return texelFetch(RtSampler, ivec2(gl_FragCoord.xy), 0);
}

// Raw source texel for channel/shuffle effects
vec4 fetch_raw_color(ivec2 xy)
{
	if (sel_tex_is_fb != 0u)
		return sample_from_rt();
	return texelFetch(Texture, xy, 0);
}

int fetch_raw_depth(ivec2 xy)
{
	vec4 col = (sel_tex_is_fb != 0u) ? sample_from_rt() : texelFetch(Texture, xy, 0);
	return int(col.r * exp2(32.0f));
}

vec4 fetch_c(ivec2 uv)
{
	if (sel_tex_is_fb != 0u)
		return sample_from_rt();
	return texelFetch(Texture, uv, 0);
}

// Palette lookup by a normalised 0..1 index
vec4 sample_p_norm(float u)
{
	return texelFetch(Palette, ivec2(int(u * 255.5f), 0), 0);
}

vec4 fetch_red(ivec2 xy)
{
	vec4 rt = (sel_depth_fmt == 1u || sel_depth_fmt == 2u)
		? vec4(float(fetch_raw_depth(xy) & 0xFF) / 255.0f)
		: fetch_raw_color(xy);
	return sample_p_norm(rt.r) * 255.0f;
}

vec4 fetch_green(ivec2 xy)
{
	vec4 rt = (sel_depth_fmt == 1u || sel_depth_fmt == 2u)
		? vec4(float((fetch_raw_depth(xy) >> 8) & 0xFF) / 255.0f)
		: fetch_raw_color(xy);
	return sample_p_norm(rt.g) * 255.0f;
}

vec4 fetch_blue(ivec2 xy)
{
	vec4 rt = (sel_depth_fmt == 1u || sel_depth_fmt == 2u)
		? vec4(float((fetch_raw_depth(xy) >> 16) & 0xFF) / 255.0f)
		: fetch_raw_color(xy);
	return sample_p_norm(rt.b) * 255.0f;
}

vec4 fetch_alpha(ivec2 xy) { return sample_p_norm(fetch_raw_color(xy).a) * 255.0f; }

vec4 fetch_rgb(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	return vec4(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0f) * 255.0f;
}

vec4 fetch_gXbY(ivec2 xy)
{
	// ChannelShuffle = (blue_mask, blue_shift, green_mask, green_shift).
	if (sel_depth_fmt == 1u || sel_depth_fmt == 2u)
	{
		int depth = fetch_raw_depth(xy);
		int bg = (depth >> (8 + ChannelShuffle.w)) & 0xFF;
		return vec4(float(bg));
	}
	uvec4 rt = uvec4(fetch_raw_color(xy) * 255.5f);
	uint green = (rt.g >> uint(ChannelShuffle.w)) & uint(ChannelShuffle.z);
	uint blue = (rt.b >> uint(ChannelShuffle.y)) & uint(ChannelShuffle.x);
	return vec4(float(green | blue));
}

vec4 sample_c(vec2 uv)
{
	// tex_is_fb samples the render target directly, e.g. BIOS clock refraction.
	if (sel_tex_is_fb != 0u)
		return sample_from_rt();

	if (sel_region_rect != 0u)
		return texelFetch(Texture, ivec2(uv), 0);

	if (sel_adjs == 0u && sel_adjt == 0u)
	{
		uv *= STScale;
	}
	else
	{
		uv.x = (sel_adjs != 0u) ? (uv.x - STRange.x) * STRange.z : uv.x * STScale.x;
		uv.y = (sel_adjt != 0u) ? (uv.y - STRange.y) * STRange.w : uv.y * STScale.y;
	}

	// Automatic (derivative) LOD, PS2 manual LOD (K/L), or base
	if (sel_automatic_lod != 0u)
		return texture(Texture, uv);
	if (sel_manual_lod != 0u)
	{
		float K = LODParams.x;
		float L = LODParams.y;
		float bias = LODParams.z;
		float max_lod = LODParams.w;
		float gs_lod = K - log2(abs(v_t.w)) * L;
		float lod = min(gs_lod, max_lod) - bias;
		return textureLod(Texture, uv, lod);
	}
	return textureLod(Texture, uv, 0.0f);
}

vec4 sample_p(uint idx)
{
	return texelFetch(Palette, ivec2(int(idx), 0), 0);
}

vec4 clamp_wrap_uv(vec4 uv)
{
	vec4 tex_size = WH.xyxy;

	if (sel_wms == sel_wmt)
	{
		if (sel_wms == 2u)
		{
			uv = clamp(uv, MinMax.xyxy, MinMax.zwzw);
		}
		else if (sel_wms == 3u)
		{
			if (sel_fst == 0u)
				uv = fract(uv);
			uv = vec4((uvec4(uv * tex_size) & floatBitsToUint(MinMax.xyxy)) | floatBitsToUint(MinMax.zwzw)) / tex_size;
		}
	}
	else
	{
		if (sel_wms == 2u)
		{
			uv.xz = clamp(uv.xz, MinMax.xx, MinMax.zz);
		}
		else if (sel_wms == 3u)
		{
			if (sel_fst == 0u)
				uv.xz = fract(uv.xz);
			uv.xz = vec2((uvec2(uv.xz * tex_size.xx) & floatBitsToUint(MinMax.xx)) | floatBitsToUint(MinMax.zz)) / tex_size.xx;
		}

		if (sel_wmt == 2u)
		{
			uv.yw = clamp(uv.yw, MinMax.yy, MinMax.ww);
		}
		else if (sel_wmt == 3u)
		{
			if (sel_fst == 0u)
				uv.yw = fract(uv.yw);
			uv.yw = vec2((uvec2(uv.yw * tex_size.yy) & floatBitsToUint(MinMax.yy)) | floatBitsToUint(MinMax.ww)) / tex_size.yy;
		}
	}

	if (sel_region_rect != 0u)
		uv = clamp(uv * WH.zwzw + STRange.xyxy, STRange.xyxy, STRange.zwzw);

	return uv;
}

mat4 sample_4c(vec4 uv)
{
	mat4 c;
	c[0] = sample_c(uv.xy);
	c[1] = sample_c(uv.zy);
	c[2] = sample_c(uv.xw);
	c[3] = sample_c(uv.zw);
	return c;
}

uvec4 sample_4_index(vec4 uv)
{
	vec4 c;
	c.x = sample_c(uv.xy).a;
	c.y = sample_c(uv.zy).a;
	c.z = sample_c(uv.xw).a;
	c.w = sample_c(uv.zw).a;

	uvec4 i = uvec4(c * 255.5f);

	if (sel_pal_fmt == 1u)
		return i & 0xFu;        // 4HL
	else if (sel_pal_fmt == 2u)
		return i >> 4u;         // 4HH
	return i;                   // 8
}

mat4 sample_4p(uvec4 u)
{
	mat4 c;
	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);
	return c;
}

// Integer UV wrap/clamp for depth sampling
ivec2 clamp_wrap_uv_depth(ivec2 uv)
{
	ivec4 mask = floatBitsToInt(MinMax) << 4;
	if (sel_wms == sel_wmt)
	{
		if (sel_wms == 2u)
			uv = clamp(uv, mask.xy, mask.zw);
		else if (sel_wms == 3u)
			uv = (uv & mask.xy) | mask.zw;
	}
	else
	{
		if (sel_wms == 2u)
			uv.x = clamp(uv.x, mask.x, mask.z);
		else if (sel_wms == 3u)
			uv.x = (uv.x & mask.x) | mask.z;

		if (sel_wmt == 2u)
			uv.y = clamp(uv.y, mask.y, mask.w);
		else if (sel_wmt == 3u)
			uv.y = (uv.y & mask.y) | mask.w;
	}
	return uv;
}

// Convert depth source to the colour the GS would read.
vec4 sample_depth(vec2 st, ivec2 pos)
{
	vec2 uv_f = vec2(clamp_wrap_uv_depth(ivec2(st))) * vec2(ScaledScaleFactor);
	if (sel_region_rect != 0u)
		uv_f = clamp(uv_f + STRange.xy, STRange.xy, STRange.zw);

	ivec2 uv = ivec2(uv_f);
	vec4 t = vec4(0.0f);

	if (sel_tales != 0u)
	{
		// Palette converts the MSB.
		int depth = fetch_raw_depth(pos);
		t = texelFetch(Palette, ivec2((depth >> 8) & 0xFF, 0), 0) * 255.0f;
	}
	else if (sel_urban_chaos != 0u)
	{
		// Depth read as RGB5A1: lsb via the palette, msb shifted into green.
		int depth = fetch_raw_depth(pos);
		t = texelFetch(Palette, ivec2(depth & 0xFF, 0), 0) * 255.0f;
		t.g += min(float(((depth >> 8) & 0xFF) * 36), 255.0f);
	}
	else if (sel_depth_fmt == 1u)
	{
		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = vec4(uvec4((d & 0xFFu), ((d >> 8) & 0xFFu), ((d >> 16) & 0xFFu), (d >> 24)));
	}
	else if (sel_depth_fmt == 2u)
	{
		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = vec4(uvec4((d & 0x1Fu), ((d >> 5) & 0x1Fu), ((d >> 10) & 0x1Fu), (d >> 15) & 0x01u)) *
			vec4(8.0f, 8.0f, 8.0f, 128.0f);
	}
	else if (sel_depth_fmt == 3u)
	{
		t = fetch_c(uv) * 255.0f;
	}

	if (sel_aem_fmt == FMT_24)
		t.a = ((sel_aem == 0u) || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	else if (sel_aem_fmt == FMT_16)
		t.a = (t.a >= 128.0f) ? 255.0f * TA.y :
			(((sel_aem == 0u) || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f);
	else if (sel_pal_fmt != 0u && sel_tales == 0u && sel_urban_chaos == 0u)
		t = trunc(sample_p(uint(t.a)) * 255.0f + 0.05f);

	return t;
}

vec4 sample_color(vec2 st)
{
	if (sel_tcoffsethack != 0u)
		st += TC_OffsetHack.xy;

	mat4 c;
	vec2 dd = vec2(0.0f);

	// Hardware-filtered single tap for the upstream fast path.
	bool fast_path = (sel_ltf == 0u && sel_aem_fmt == FMT_32 && sel_pal_fmt == 0u &&
		sel_region_rect == 0u && sel_wms < 2u && sel_wmt < 2u);

	if (fast_path)
	{
		c[0] = sample_c(st);
	}
	else
	{
		vec4 uv;
		if (sel_ltf != 0u)
		{
			uv = st.xyxy + HalfTexel;
			dd = fract(uv.xy * WH.zw);
			if (sel_fst == 0u)
				dd = clamp(dd, vec2(0.0f), vec2(0.9999999f));
		}
		else
		{
			uv = st.xyxy;
		}

		uv = clamp_wrap_uv(uv);

		if (sel_pal_fmt != 0u)
			c = sample_4p(sample_4_index(uv));
		else
			c = sample_4c(uv);
	}

	// AEM alpha expansion per tap
	for (int i = 0; i < 4; i++)
	{
		if (sel_aem_fmt == FMT_24)
			c[i].a = (sel_aem == 0u || any(bvec3(c[i].rgb))) ? TA.x : 0.0f;
		else if (sel_aem_fmt == FMT_16)
			c[i].a = (c[i].a >= 0.5f) ? TA.y :
				((sel_aem == 0u || any(bvec3(ivec3(c[i].rgb * 255.0f) & ivec3(0xF8)))) ? TA.x : 0.0f);
	}

	vec4 t = (sel_ltf != 0u) ? mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y) : c[0];

	return trunc(t * 255.0f + 0.05f);
}

vec4 tfx(vec4 T, vec4 C)
{
	vec4 C_out;
	vec4 FxT = trunc((C * T) / 128.0f);

	if (sel_tfx == 0u)
		C_out = FxT;
	else if (sel_tfx == 1u)
		C_out = T;
	else if (sel_tfx == 2u)
	{
		C_out.rgb = FxT.rgb + C.a;
		C_out.a = T.a + C.a;
	}
	else if (sel_tfx == 3u)
	{
		C_out.rgb = FxT.rgb + C.a;
		C_out.a = T.a;
	}
	else
		C_out = C;

	if (sel_tcc == 0u)
		C_out.a = C.a;

	if (sel_tfx == 0u || sel_tfx == 2u || sel_tfx == 3u)
		C_out = min(C_out, 255.0f);

	return C_out;
}

bool atst(vec4 C)
{
	float a = C.a;
	if (sel_atst == 1u)
		return (a <= AREF);
	else if (sel_atst == 2u)
		return (a >= AREF);
	else if (sel_atst == 3u)
		return (abs(a - AREF) <= 0.5f);
	else if (sel_atst == 4u)
		return (abs(a - AREF) >= 0.5f);
	return true;
}

vec4 fog(vec4 c, float f)
{
	if (sel_fog != 0u)
		c.rgb = trunc(mix(FogColor, c.rgb, f));
	return c;
}

vec4 ps_color()
{
	vec2 st = (sel_fst != 0u) ? v_ti.xy : (v_t.xy / v_t.w);
	vec2 st_int = (sel_fst != 0u) ? v_ti.zw : (v_ti.zw / v_t.w);
	ivec2 fc = ivec2(gl_FragCoord.xy);

	vec4 T;
	if (sel_channel == 1u)
		T = fetch_red(fc);
	else if (sel_channel == 2u)
		T = fetch_green(fc);
	else if (sel_channel == 3u)
		T = fetch_blue(fc);
	else if (sel_channel == 4u)
		T = fetch_alpha(fc);
	else if (sel_channel == 5u)
		T = fetch_rgb(fc);
	else if (sel_channel == 6u)
		T = fetch_gXbY(fc);
	else if (sel_depth_fmt != 0u || sel_urban_chaos != 0u || sel_tales != 0u)
		T = sample_depth(st_int, fc);
	else
		T = (sel_tme != 0u) ? sample_color(st) : vec4(0.0f);

	// Expand sampled RGBA8 back into 16-bit halves
	if (sel_shuffle != 0u && sel_shuffle_same == 0u && sel_read16src == 0u &&
		!(sel_process_ba == 3u && sel_process_rg == 3u))
	{
		uvec4 denorm_c = uvec4(T);
		if ((sel_process_ba & 1u) != 0u)
		{
			T.r = float((denorm_c.b << 3) & 0xF8u);
			T.g = float(((denorm_c.b >> 2) & 0x38u) | ((denorm_c.a << 6) & 0xC0u));
			T.b = float((denorm_c.a << 1) & 0xF8u);
			T.a = float(denorm_c.a & 0x80u);
		}
		else
		{
			T.r = float((denorm_c.r << 3) & 0xF8u);
			T.g = float(((denorm_c.r >> 2) & 0x38u) | ((denorm_c.g << 6) & 0xC0u));
			T.b = float((denorm_c.g << 1) & 0xF8u);
			T.a = float(denorm_c.g & 0x80u);
		}

		T.a = (T.a >= 127.5f ? TA.y :
			((sel_aem == 0u || any(notEqual(ivec3(T.rgb) & ivec3(0xF8), ivec3(0)))) ? TA.x : 0.0f)) * 255.0f;
	}

	vec4 C = tfx(T, (sel_iip != 0u) ? v_c : v_cf);
	return fog(C, v_t.z);
}

// Software blend plus hardware colour/alpha adjustments
void ps_blend(inout vec4 Color, inout vec4 As_rgba)
{
	bool sw_blend = (sel_blend_a != 0u || sel_blend_b != 0u || sel_blend_d != 0u);
	float As = As_rgba.a;

	if (sw_blend)
	{
		if (sel_pabe != 0u)
		{
			if (As < 1.0f)
			{
				As_rgba.rgb = vec3(0.0f);
				return;
			}
			As_rgba.rgb = vec3(1.0f);
		}

		vec4 RT = sample_from_rt();
		float Ad = (sel_rta_correction != 0u) ? trunc(RT.a * 128.0f + 0.1f) / 128.0f
											   : trunc(RT.a * 255.0f + 0.1f) / 128.0f;
		vec3 Cd = (sel_colclip_hw != 0u) ? trunc(RT.rgb * 65535.0f) : trunc(RT.rgb * 255.0f + 0.1f);
		vec3 Cs = Color.rgb;

		vec3 A = (sel_blend_a == 0u) ? Cs : (sel_blend_a == 1u) ? Cd : vec3(0.0f);
		vec3 B = (sel_blend_b == 0u) ? Cs : (sel_blend_b == 1u) ? Cd : vec3(0.0f);
		float C = (sel_blend_c == 0u) ? As : (sel_blend_c == 1u) ? Ad : Af;
		vec3 D = (sel_blend_d == 0u) ? Cs : (sel_blend_d == 1u) ? Cd : vec3(0.0f);

		float C_clamped = C;
		if (sel_blend_mix > 0u && sel_blend_hw != 1u && sel_blend_hw != 2u)
			C_clamped = min(C_clamped, 1.0f);

		if (sel_blend_a == sel_blend_b)
			Color.rgb = D;
		else if (sel_blend_mix == 2u)
			Color.rgb = ((A - B) * C_clamped + D) + (124.0f / 256.0f);
		else if (sel_blend_mix == 1u)
			Color.rgb = ((A - B) * C_clamped + D) - (124.0f / 256.0f);
		else
			Color.rgb = trunc((A - B) * C + D);

		if (sel_blend_hw == 1u)
		{
			As_rgba.rgb = vec3(C);
			As_rgba.rgb -= max(vec3(1.0f), Color.rgb / vec3(255.0f));
		}
		else if (sel_blend_hw == 2u)
		{
			Color.rgb /= vec3(1.0f + C);
		}
		else if (sel_blend_hw == 3u)
		{
			As_rgba.rgb = vec3(C_clamped);
			As_rgba.rgb -= max(vec3(0.0f), (Color.rgb - vec3(255.0f)) / 255.0f);
		}
	}
	else
	{
		vec3 Alpha = (sel_blend_c == 2u) ? vec3(Af) : vec3(As);

		if (sel_blend_hw == 1u)
		{
			Color.rgb = vec3(255.0f);
		}
		else if (sel_blend_hw == 2u)
		{
			Color.rgb = max(vec3(0.0f), (Alpha - vec3(1.0f))) * vec3(255.0f);
		}
		else if (sel_blend_hw == 3u && sel_rta_correction == 0u)
		{
			float max_color = max(max(Color.r, Color.g), Color.b);
			Color.rgb *= vec3(255.0f / max(128.0f, max_color));
		}
		else if (sel_blend_hw == 4u)
		{
			As_rgba.rgb = Alpha * vec3(128.0f / 255.0f);
			Color.rgb = vec3(127.5f);
		}
		else if (sel_blend_hw == 5u)
		{
			Alpha *= vec3(128.0f / 255.0f);
			As_rgba.rgb = (Alpha - vec3(0.5f));
			Color.rgb = (Color.rgb * Alpha);
		}
		else if (sel_blend_hw == 6u)
		{
			Alpha *= vec3(128.0f / 255.0f);
			As_rgba.rgb = Alpha;
			Color.rgb *= (Alpha - vec3(0.5f));
		}
	}
}

// Partial-channel framebuffer mask
void ps_fbmask(inout vec4 C)
{
	if (sel_fbmask != 0u)
	{
		float multi = (sel_colclip_hw != 0u) ? 65535.0f : 255.5f;
		vec4 RT = sample_from_rt();
		C = vec4((uvec4(ivec4(C)) & (FbMask ^ uvec4(0xFFu))) |
			(uvec4(RT * vec4(multi, multi, multi, 255.0f)) & FbMask));
	}
}

void ps_dither(inout vec3 C, float As)
{
	if (sel_dither > 0u && sel_dither < 3u)
	{
		ivec2 fpos = (sel_dither == 2u) ? ivec2(gl_FragCoord.xy) : ivec2(gl_FragCoord.xy * RcpScaleFactor);
		float value = DitherMatrix[fpos.y & 3][fpos.x & 3];

		if (sel_dither_adjust != 0u)
		{
			float Alpha = (sel_blend_c == 2u) ? Af : As;
			value *= Alpha > 0.0f ? min(1.0f / Alpha, 1.0f) : 1.0f;
		}

		if (sel_round_inv != 0u)
			C -= value;
		else
			C += value;
	}
}

void ps_color_clamp_wrap(inout vec3 C)
{
	bool sw_blend = (sel_blend_a != 0u || sel_blend_b != 0u || sel_blend_d != 0u);
	bool dithering = (sel_dither > 0u && sel_dither < 3u);

	if (sw_blend || dithering || sel_fbmask != 0u)
	{
		if (sel_dst_fmt == FMT_16 && sel_blend_mix == 0u && sel_round_inv != 0u)
			C += 7.0f;

		if (sel_colclip == 0u && sel_colclip_hw == 0u)
			C = clamp(C, vec3(0.0f), vec3(255.0f));

		if (sel_dst_fmt == FMT_16 && sel_dither != 3u && (sel_blend_mix == 0u || sel_dither > 0u))
			C = vec3(ivec3(C) & ivec3(0xF8));
		else if (sel_colclip == 1u || sel_colclip_hw == 1u)
			C = vec3(ivec3(C) & ivec3(0xFF));
	}
	else if (sel_dst_fmt == FMT_16 && sel_dither != 3u && sel_blend_mix == 0u && sel_blend_hw == 0u)
	{
		C = vec3(ivec3(C) & ivec3(0xF8));
	}
}

void main()
{
	if ((sel_scanmsk & 2u) != 0u)
	{
		if ((uint(gl_FragCoord.y) & 1u) == (sel_scanmsk & 1u))
			discard;
	}

	// DATE >= 5 reads current alpha from the RT (in-shader full barrier DATE)
	if (sel_date >= 5u)
	{
		vec4 RTd = sample_from_rt();
		float rt_a = (sel_write_rg != 0u) ? RTd.g : RTd.a;
		bool bad = (sel_rta_correction != 0u)
			? (((sel_date & 3u) == 1u) ? (rt_a > (254.5f / 255.0f)) : (rt_a < (254.5f / 255.0f)))
			: (((sel_date & 3u) == 1u) ? (rt_a > 0.5f) : (rt_a < 0.5f));
		if (bad)
			discard;
	}

	// PrimID tracking (DATE 3)
	if (sel_date == 3u)
	{
		int stencil_ceil = int(texelFetch(PrimMinTexture, ivec2(gl_FragCoord.xy), 0).r);
		if (gl_PrimitiveID > stencil_ceil)
			discard;
	}

	vec4 C = ps_color();
	bool atst_pass = atst(C);

	if (sel_afail == 0u && !atst_pass)
		discard;

	// AA outputs a coverage of 1.0 as alpha
	if (sel_fixed_one_a != 0u)
		C.a = 128.0f;

	vec4 alpha_blend;
	if (sel_blend_c == 1u && sel_a_masked != 0u)
	{
		vec4 RT = (sel_rta_correction != 0u) ? trunc(sample_from_rt() * 128.0f + 0.1f)
											  : trunc(sample_from_rt() * 255.0f + 0.1f);
		alpha_blend = vec4(RT.a / 128.0f);
	}
	else
	{
		alpha_blend = vec4(C.a / 128.0f);
	}

	// Correct the alpha value based on the output format
	if (sel_dst_fmt == FMT_16)
	{
		float A_one = 128.0f;
		C.a = (sel_fba != 0u) ? A_one : step(128.0f, C.a) * A_one;
	}
	else if (sel_dst_fmt == FMT_32 && sel_fba != 0u)
	{
		if (C.a < 128.0f)
			C.a += 128.0f;
	}

	// Emit the primitive id where this fragment would write a failing alpha else INT_MAX (DATE 1/2)
	if (sel_date == 1u)
	{
		// alpha >= 0x80 fails
		o_col0 = (C.a > 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);
		return;
	}
	else if (sel_date == 2u)
	{
		// alpha < 0x80 fails
		o_col0 = (C.a < 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);
		return;
	}

	ps_blend(C, alpha_blend);

	if (sel_shuffle != 0u)
	{
		if (sel_shuffle_same == 0u && sel_read16src == 0u &&
			!(sel_process_ba == 3u && sel_process_rg == 3u))
		{
			uvec4 denorm_c = uvec4(C);
			if ((sel_process_ba & 1u) != 0u)
			{
				C.b = float(((denorm_c.r >> 3) & 0x1Fu) | ((denorm_c.g << 2) & 0xE0u));
				C.a = float(((denorm_c.g >> 6) & 0x3u) | ((denorm_c.b >> 1) & 0x7Cu) | (denorm_c.a & 0x80u));
			}
			else
			{
				C.r = float(((denorm_c.r >> 3) & 0x1Fu) | ((denorm_c.g << 2) & 0xE0u));
				C.g = float(((denorm_c.g >> 6) & 0x3u) | ((denorm_c.b >> 1) & 0x7Cu) | (denorm_c.a & 0x80u));
			}
		}

		if (sel_shuffle_same != 0u)
		{
			uvec4 denorm_c = uvec4(C);
			if ((sel_process_ba & 1u) != 0u)
				C = vec4(float((denorm_c.b & 0x7Fu) | (denorm_c.a & 0x80u)));
			else
				C.ga = C.rg;
		}
		else if (sel_read16src != 0u)
		{
			uvec4 denorm_c = uvec4(C);
			uvec2 denorm_TA = uvec2(TA * 255.5f);
			C.rb = vec2(float((denorm_c.r >> 3) | (((denorm_c.g >> 3) & 0x7u) << 5)));
			if ((denorm_c.a & 0x80u) != 0u)
				C.ga = vec2(float((denorm_c.g >> 6) | ((denorm_c.b >> 3) << 2) | (denorm_TA.y & 0x80u)));
			else
				C.ga = vec2(float((denorm_c.g >> 6) | ((denorm_c.b >> 3) << 2) | (denorm_TA.x & 0x80u)));
		}
		else if (sel_shuffle_across != 0u)
		{
			if (sel_process_ba == 3u && sel_process_rg == 3u)
			{
				C.br = C.rb;
				C.ag = C.ga;
			}
			else if ((sel_process_ba & 1u) != 0u)
			{
				C.rb = C.bb;
				C.ga = C.aa;
			}
			else
			{
				C.rb = C.rr;
				C.ga = C.gg;
			}
		}
	}

	ps_dither(C.rgb, alpha_blend.a);

	ps_color_clamp_wrap(C.rgb);

	ps_fbmask(C);

	// Use the alpha blend factor to gate the alpha write.
	if (sel_afail == 3u)
		alpha_blend.a = float(atst_pass);

	o_col0.a = (sel_rta_correction != 0u) ? (C.a / 128.0f) : (C.a / 255.0f);
	o_col0.rgb = (sel_colclip_hw == 1u) ? (C.rgb / 65535.0f) : (C.rgb / 255.0f);
	o_col1 = alpha_blend;
}
