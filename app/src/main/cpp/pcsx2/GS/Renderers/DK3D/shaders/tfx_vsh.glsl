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
	uint pad_cb0;
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
};

layout(location = 0) in vec2 a_st;
layout(location = 1) in uvec4 a_c;
layout(location = 2) in float a_q;
layout(location = 3) in uvec2 a_p;
layout(location = 4) in uint a_z;
layout(location = 5) in uvec2 a_uv;
layout(location = 6) in vec4 a_f;

layout(location = 0) out vec4 v_t;
layout(location = 1) out vec4 v_ti;
layout(location = 2) out vec4 v_c;
layout(location = 3) flat out vec4 v_cf;

void main()
{
	uint z = min(a_z, MaxDepth);

	gl_Position = vec4(vec2(a_p), float(z), 1.0f) - vec4(0.05f, 0.05f, 0.0f, 0.0f);
	gl_Position.xy = gl_Position.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	gl_Position.z *= exp2(-32.0f); // integer -> float depth
	// deko3d is Y-up, Vulkan is Y-down
	// DO NOT FORGET THIS AGAIN

	if (sel_tme != 0u)
	{
		vec2 uv = vec2(a_uv) - TextureOffset;
		vec2 st = a_st - TextureOffset;

		v_ti.xy = uv * TextureScale;
		if (sel_fst != 0u)
			v_ti.zw = uv;
		else
			v_ti.zw = st / TextureScale;

		v_t.xy = st;
		v_t.w = a_q;
	}
	else
	{
		v_t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		v_ti = vec4(0.0f);
	}

	v_c = vec4(a_c);
	v_cf = vec4(a_c);
	v_t.z = a_f.r;
}
