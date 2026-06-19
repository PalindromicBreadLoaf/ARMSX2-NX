#version 460

// Deinterlacing ubershader
// u_mode matches ShaderInterlace (0 weave, 1 bob, 2 blend, 3 MAD buffer, 4 MAD reconstruct)

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

layout(binding = 0) uniform sampler2D samp0;

layout(std140, binding = 0) uniform cb0
{
	vec4 ZrH; // x: buffer index, y: 1/vres, z: vres, w: MAD sensitivity
	uint u_mode;
};

void mode_weave()
{
	const int idx   = int(ZrH.x);
	const int field = idx & 1;
	const int vpos  = int(gl_FragCoord.y);

	if ((vpos & 1) == field)
		oColor = textureLod(samp0, vTexCoord, 0.0);
	else
		discard;
}

void mode_bob()
{
	oColor = textureLod(samp0, vTexCoord, 0.0);
}

void mode_blend()
{
	vec2 vstep = vec2(0.0, ZrH.y);
	vec4 c0 = textureLod(samp0, vTexCoord - vstep, 0.0);
	vec4 c1 = textureLod(samp0, vTexCoord, 0.0);
	vec4 c2 = textureLod(samp0, vTexCoord + vstep, 0.0);

	oColor = (c0 + c1 * 2.0 + c2) / 4.0;
}

void mode_mad_buffer()
{
	const int idx   = int(ZrH.x);
	const int bank  = idx >> 1;
	const int field = idx & 1;
	const int vres  = int(ZrH.z) >> 1;
	const int lofs  = ((((vres + 1) >> 1) << 1) - vres) & bank;
	const int vpos  = int(gl_FragCoord.y) + lofs;

	if ((vpos & 1) == field)
		oColor = textureLod(samp0, vTexCoord, 0.0);
	else
		discard;
}

void mode_mad_reconstruct()
{
	const int   idx         = int(ZrH.x);
	const int   field       = idx & 1;
	const int   vpos        = int(gl_FragCoord.y);
	const float sensitivity = ZrH.w;
	const vec3  motion_thr  = vec3(1.0, 1.0, 1.0) * sensitivity;
	const vec2  bofs        = vec2(0.0, 0.5);
	const vec2  vscale      = vec2(1.0, 0.5);
	const vec2  lofs        = vec2(0.0, ZrH.y) * vscale;
	const vec2  iptr        = vTexCoord * vscale;

	vec2 p_t0;
	vec2 p_t1;
	vec2 p_t2;
	vec2 p_t3;

	switch (idx)
	{
		case 1:
			p_t0 = iptr;
			p_t1 = iptr;
			p_t2 = iptr + bofs;
			p_t3 = iptr + bofs;
			break;
		case 2:
			p_t0 = iptr + bofs;
			p_t1 = iptr;
			p_t2 = iptr;
			p_t3 = iptr + bofs;
			break;
		case 3:
			p_t0 = iptr + bofs;
			p_t1 = iptr + bofs;
			p_t2 = iptr;
			p_t3 = iptr;
			break;
		default:
			p_t0 = iptr;
			p_t1 = iptr + bofs;
			p_t2 = iptr + bofs;
			p_t3 = iptr;
			break;
	}

	vec4 hn = textureLod(samp0, p_t0 - lofs, 0.0);
	vec4 cn = textureLod(samp0, p_t1, 0.0);
	vec4 ln = textureLod(samp0, p_t0 + lofs, 0.0);

	vec4 ho = textureLod(samp0, p_t2 - lofs, 0.0);
	vec4 co = textureLod(samp0, p_t3, 0.0);
	vec4 lo = textureLod(samp0, p_t2 + lofs, 0.0);

	vec3 mh = hn.rgb - ho.rgb;
	vec3 mc = cn.rgb - co.rgb;
	vec3 ml = ln.rgb - lo.rgb;

	mh = max(mh, -mh) - motion_thr;
	mc = max(mc, -mc) - motion_thr;
	ml = max(ml, -ml) - motion_thr;

	float mh_max = max(max(mh.x, mh.y), mh.z);
	float mc_max = max(max(mc.x, mc.y), mc.z);
	float ml_max = max(max(ml.x, ml.y), ml.z);

	if ((vpos & 1) == field)
	{
		oColor = textureLod(samp0, p_t0, 0.0);
	}
	else if ((iptr.y > 0.5 - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		// Top and bottom lines are always weaved.
		oColor = cn;
	}
	else
	{
		// Reconstruct the missing line.
		if (((mh_max > 0.0) || (ml_max > 0.0)) || (mc_max > 0.0))
			oColor = (hn + ln) / 2.0; // high motion
		else
			oColor = cn;              // low motion
	}
}

void main()
{
	if (u_mode == 0u)
		mode_weave();
	else if (u_mode == 1u)
		mode_bob();
	else if (u_mode == 2u)
		mode_blend();
	else if (u_mode == 3u)
		mode_mad_buffer();
	else
		mode_mad_reconstruct();
}
