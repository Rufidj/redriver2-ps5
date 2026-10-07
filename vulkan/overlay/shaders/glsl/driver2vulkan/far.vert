#version 450
#extension GL_GOOGLE_include_directive : require
// The far field: static world-space meshes (game/libred2vk.a's farmesh.c) projected here as the GTE and the PGXP vertex
// shader (psx.vert) would, so that it matches the game's own drawing at the seam. The varyings are psx.vert's: the same
// fragment shader textures it.
layout(location = 0) in vec3 a_pos;      // relative to the chunk's origin (world units)
layout(location = 1) in uvec2 a_pc;      // tpage, clut
layout(location = 2) in uvec4 a_uvc;     // u, v, colour selector, flags (1 water/grass ground, 2 ground)

#include "fx.glsl"

layout(location = 0) out vec4 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_page_clut;
layout(location = 3) out float v_is3D;
layout(location = 4) out vec4 v_shadowPos;
layout(location = 5) out float v_fogDepth;
layout(location = 6) out float v_hdKey;

const vec2 c_UVFudge = vec2(0.00025, 0.00025);

void main()
{
	// the chunk's origin minus the camera, in world units, comes with the draw (the push constants' spare words)
	vec3 off = vec3(pc.texelSize, intBitsToFloat(pc.pad));
	vec3 d = a_pos + off;                       // camera-relative world position
	vec3 V = d * fx.viewToWorld;                // the game's view space (viewToWorld is its transpose)
	float zs = ((a_uvc.w & 2u) != 0u) ? (((a_uvc.w & 1u) != 0u) ? 1.005 : 0.995) : 1.0;   // ground layering, as Tile1x1
	float Z = V.z / 128.0 * zs;
	float X = V.x / 128.0 * fx.skyH / fx.dispW;
	float Y = -(V.y / 128.0 * fx.skyH / fx.dispH);
	vec4 p = fx.Projection3D * vec4(X, Y, Z, 1.0);
	p.xy += vec2(fx.farOfsX, -fx.farOfsY) * p.w;
	p.z = (p.z + p.w) * 0.5;
	gl_Position = p;

	// flag 4: a sprite (a tree, a lamp): its transparent texels are dropped even where the far field keeps them
	v_texcoord = vec4(float(a_uvc.x), float(a_uvc.y), 2.0, ((a_uvc.w & 4u) != 0u) ? 1.0 : 0.0);
	uint cw = fx.farCol[a_uvc.z >> 2][a_uvc.z & 3u];
	v_color = vec4(float(cw & 255u), float((cw >> 8) & 255u), float((cw >> 16) & 255u), 255.0) / 255.0;
	v_color.xyz *= 2.0;
	// the far field's page store: the page id and the palette index, as they are
	v_page_clut = vec4(float(a_pc.x), float(a_pc.y), 0.0, 0.0);
	v_is3D = 1.0;
	v_fogDepth = Z;
	v_hdKey = 0.0;
	v_shadowPos = vec4(d, 1.0);
}
