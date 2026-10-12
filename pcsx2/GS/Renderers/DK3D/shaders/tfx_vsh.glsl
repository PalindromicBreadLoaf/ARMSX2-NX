#version 460

// Runtime-selected tfx vertex shader

layout(std140, binding = 1) uniform cb0
{
	vec2 VertexScale;
	vec2 VertexOffset;
	vec2 TextureScale;
	vec2 TextureOffset;
	vec2 PointSize;
	uint MaxDepth;
	float LineAA1Width;
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

#if defined(TFX_VS_EXPAND_POINT) || defined(TFX_VS_EXPAND_LINE) || defined(TFX_VS_EXPAND_LINE_AA1) || \
	defined(TFX_VS_EXPAND_SPRITE)
#define TFX_VS_EXPAND 1
#endif

layout(location = 0) out vec4 v_t;
layout(location = 1) out vec4 v_ti;
layout(location = 2) out vec4 v_c;
layout(location = 3) flat out vec4 v_cf;
#if defined(TFX_VS_EXPAND_LINE_AA1)
layout(location = 4) out float v_inv_cov;
#endif

struct ProcessedVertex
{
	vec4 p;
	vec4 t;
	vec4 ti;
	vec4 c;
};

ProcessedVertex process_vertex(vec2 a_st, uvec4 a_c, float a_q, uvec2 a_p, uint a_z, uvec2 a_uv, float a_fog)
{
	ProcessedVertex vtx;

	uint z = min(a_z, MaxDepth);

	vtx.p = vec4(vec2(a_p), float(z), 1.0f) - vec4(0.05f, 0.05f, 0.0f, 0.0f);
	vtx.p.xy = vtx.p.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	vtx.p.z *= exp2(-32.0f);

	if (sel_tme != 0u)
	{
		vec2 uv = vec2(a_uv) - TextureOffset;
		vec2 st = a_st - TextureOffset;

		vtx.ti.xy = uv * TextureScale;
		if (sel_fst != 0u)
			vtx.ti.zw = uv;
		else
			vtx.ti.zw = st / TextureScale;

		vtx.t.xy = st;
		vtx.t.w = a_q;
	}
	else
	{
		vtx.t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		vtx.ti = vec4(0.0f);
	}

	vtx.c = vec4(a_c);
	vtx.t.z = a_fog;
	return vtx;
}

#if defined(TFX_VS_EXPAND)

// Matches GSVertex
struct RawVertex
{
	vec2 ST;
	uint RGBA;
	float Q;
	uint XY;
	uint Z;
	uint UV;
	uint FOG;
};

// The buffer is bound at the draw's first vertex
layout(std430, binding = 0) readonly buffer VertexBuffer
{
	RawVertex vertex_buffer[];
};

ProcessedVertex load_vertex(uint index)
{
	RawVertex rvtx = vertex_buffer[index];
	uvec4 a_c = uvec4(bitfieldExtract(rvtx.RGBA, 0, 8), bitfieldExtract(rvtx.RGBA, 8, 8),
		bitfieldExtract(rvtx.RGBA, 16, 8), bitfieldExtract(rvtx.RGBA, 24, 8));
	uvec2 a_p = uvec2(bitfieldExtract(rvtx.XY, 0, 16), bitfieldExtract(rvtx.XY, 16, 16));
	uvec2 a_uv = uvec2(bitfieldExtract(rvtx.UV, 0, 16), bitfieldExtract(rvtx.UV, 16, 16));
	return process_vertex(rvtx.ST, a_c, rvtx.Q, a_p, rvtx.Z, a_uv, unpackUnorm4x8(rvtx.FOG).r);
}

void main()
{
	ProcessedVertex vtx;
	uint vid = uint(gl_VertexID);

#if defined(TFX_VS_EXPAND_POINT)

	vtx = load_vertex(vid >> 2);

	vtx.p.x += ((vid & 1u) != 0u) ? PointSize.x : 0.0f;
	vtx.p.y -= ((vid & 2u) != 0u) ? PointSize.y : 0.0f;

#elif defined(TFX_VS_EXPAND_LINE) || defined(TFX_VS_EXPAND_LINE_AA1)

	uint vid_base = vid >> 2;

	bool is_bottom = (vid & 2u) != 0u;
	bool is_right = (vid & 1u) != 0u;
	uint vid_other = is_bottom ? vid_base - 1u : vid_base + 1u;

	vtx = load_vertex(vid_base);
	ProcessedVertex other = load_vertex(vid_other);

	vec2 line_delta = is_bottom ? (vtx.p.xy - other.p.xy) : (other.p.xy - vtx.p.xy);
	vec2 line_vector = normalize(line_delta / VertexScale);
	vec2 line_expand = vec2(line_vector.y, -line_vector.x);
#if defined(TFX_VS_EXPAND_LINE_AA1)
	line_expand *= 2.0f * LineAA1Width;
#endif
	vec2 line_width = (line_expand * PointSize) / 2.0f;
	vtx.p.xy += is_right ? line_width : -line_width;

#if defined(TFX_VS_EXPAND_LINE_AA1)
	v_inv_cov = is_right ? 1.0f : -1.0f;
#endif

#elif defined(TFX_VS_EXPAND_SPRITE)

	// Sprite vertices always come in pairs
	uint vid_base = vid >> 1;
	ProcessedVertex lt = load_vertex(vid_base & ~1u);
	ProcessedVertex rb = load_vertex(vid_base | 1u);
	vtx = rb;

	bool is_right = (vid & 1u) != 0u;
	vtx.p.x = is_right ? lt.p.x : vtx.p.x;
	vtx.t.x = is_right ? lt.t.x : vtx.t.x;
	vtx.ti.xz = is_right ? lt.ti.xz : vtx.ti.xz;

	bool is_bottom = (vid & 2u) != 0u;
	vtx.p.y = is_bottom ? lt.p.y : vtx.p.y;
	vtx.t.y = is_bottom ? lt.t.y : vtx.t.y;
	vtx.ti.yw = is_bottom ? lt.ti.yw : vtx.ti.yw;

#endif

	gl_Position = vtx.p;
	v_t = vtx.t;
	v_ti = vtx.ti;
	v_c = vtx.c;
	v_cf = vtx.c;
}

#else

layout(location = 0) in vec2 a_st;
layout(location = 1) in uvec4 a_c;
layout(location = 2) in float a_q;
layout(location = 3) in uvec2 a_p;
layout(location = 4) in uint a_z;
layout(location = 5) in uvec2 a_uv;
layout(location = 6) in vec4 a_f;

void main()
{
	ProcessedVertex vtx = process_vertex(a_st, a_c, a_q, a_p, a_z, a_uv, a_f.r);
	gl_Position = vtx.p;
	v_t = vtx.t;
	v_ti = vtx.ti;
	v_c = vtx.c;
	v_cf = vtx.c;
}

#endif
