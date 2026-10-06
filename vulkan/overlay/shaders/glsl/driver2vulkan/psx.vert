#version 450
#extension GL_GOOGLE_include_directive : require
// The PSX GTE vertex stage of PsyCross, as GLSL 450 for Vulkan. The game's matrices are OpenGL's
// (clip z in -1..1, y up): z is remapped here, and the viewport is flipped by the renderer.
layout(location = 0) in vec4 a_position;   // x, y, page, clut
layout(location = 1) in vec4 a_zw;         // z, scr_h, ofsX, ofsY
layout(location = 2) in vec4 a_texcoord;   // u, v, colour multiplier, dither
layout(location = 3) in vec4 a_color;
layout(location = 4) in vec4 a_extra;      // texcoord ofs

#include "fx.glsl"

layout(location = 0) out vec4 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_page_clut;
layout(location = 3) out float v_is3D;
layout(location = 4) out vec4 v_shadowPos;   // camera-relative world position
layout(location = 5) out float v_fogDepth;
layout(location = 6) out float v_hdKey;

const vec2 c_UVFudge = vec2(0.00025, 0.00025);

void main()
{
	v_texcoord = a_texcoord;
	v_texcoord.xy += a_extra.xy * 0.5;
	v_color = a_color;
	v_color.xyz *= a_texcoord.z;
	v_page_clut.x = fract(a_position.z / 16.0) * 1024.0;
	v_page_clut.y = floor(a_position.z / 16.0) * 256.0;
	v_page_clut.z = fract(a_position.w / 64.0);
	v_page_clut.w = floor(a_position.w / 64.0) / 512.0;
	v_page_clut.xy += c_UVFudge;
	v_page_clut.zw += c_UVFudge;

	mat4 ofsMat = mat4(
		vec4(1.0, 0.0, 0.0, 0.0),
		vec4(0.0, 1.0, 0.0, 0.0),
		vec4(0.0, 0.0, 1.0, 0.0),
		vec4(a_zw.z, -a_zw.w, 0.0, 1.0));
	vec2 geom_ofs = vec2(0.5, 0.5);
	vec4 p = (a_zw.y > 100.0
		? ofsMat * (fx.Projection3D * vec4((a_position.xy + geom_ofs) * vec2(1.0, -1.0) * a_zw.y, a_zw.x, 1.0))
		: (fx.Projection * vec4(a_position.xy, 0.5, 1.0)));
	v_is3D = (a_zw.y > 100.0) ? 1.0 : 0.0;
	v_fogDepth = a_zw.x;
	v_hdKey = float(int(a_position.z + 0.5) & 31);
	v_shadowPos = vec4(0.0);
	if (v_is3D > 0.5) {
		vec3 vview = vec3((a_position.x + 0.5) * fx.dispW, (a_position.y + 0.5) * fx.dispH, a_zw.x) * 128.0;
		v_shadowPos = vec4(fx.viewToWorld * vview, 1.0);
	}
	if (pc.pass == 1)
		p = (v_is3D > 0.5) ? (((pc.cascade < 3) ? fx.lightVP[pc.cascade] : fx.spotVP[pc.cascade - 3]) * v_shadowPos) : vec4(2.0, 2.0, 2.0, 1.0);
	if (pc.texMode == 4) p = vec4(a_position.xy, 0.0, 1.0);   // the FMV quad is already in clip space
	p.z = (p.z + p.w) * 0.5;
	gl_Position = p;
}
