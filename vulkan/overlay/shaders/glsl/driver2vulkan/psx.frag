#version 450
#extension GL_GOOGLE_include_directive : require
// The PSX texture sampling of PsyCross (4/8/16-bit paletted VRAM through the RG LUT, or a plain RGBA texture),
// and the PS5 look on top of it: HD texture pages, sun cascades and headlight shadows, lights, wet roads, fog.
layout(location = 0) in vec4 v_texcoord;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_page_clut;
layout(location = 3) in float v_is3D;
layout(location = 4) in vec4 v_shadowPos;
layout(location = 5) in float v_fogDepth;
layout(location = 6) in float v_hdKey;
layout(location = 0) out vec4 fragColor;

#include "fx.glsl"

layout(set = 0, binding = 0) uniform sampler2D s_texture;
layout(set = 0, binding = 1) uniform sampler2D s_rgLut;
layout(set = 0, binding = 2) uniform sampler2DArray s_pageIdx;   // the far field's pages: 8-bit palette indices
layout(set = 0, binding = 3) uniform sampler2D s_pagePal;        // their palettes, 16 colours a row, row = page * 64 + palette
layout(set = 2, binding = 0) uniform sampler2DArrayShadow uShadowMap;
layout(set = 2, binding = 1) uniform sampler2DArrayShadow uSpotMap;
layout(set = 2, binding = 2) uniform sampler2DArray uHDTex;
layout(set = 2, binding = 3) uniform sampler2D uRefl;                 // the water's reflection (the far field mirrored)

const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);
vec2 VRAM(vec2 uv) { return texture(s_texture, uv).rg; }
float _idx2(vec2 array, int idx) { return array[idx]; }

vec2 samplePSX4(vec2 tc) {
	vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;
	vec2 comp = VRAM(uv);
	int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);
	float v = _idx2(comp, index / 2) * (255.0 / 16.0);
	float f = floor(v + 0.001);
	vec2 c = vec2((v - f) * 16.0, f);
	vec2 clut_pos = v_page_clut.zw;
	clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;
	return VRAM(clut_pos);
}
vec2 samplePSX8(vec2 tc) {
	vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;
	vec2 comp = VRAM(uv);
	vec2 clut_pos = v_page_clut.zw;
	int index = int(mod(tc.x, 2.0));
	clut_pos.x += _idx2(comp, index) * 255.0 * c_VRAMTexel.x;
	return VRAM(clut_pos);
}
vec2 samplePSX16(vec2 tc) {
	vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;
	return VRAM(uv);
}
vec2 samplePSX(vec2 tc) {
	if (pc.texMode == 0) return samplePSX4(tc);
	if (pc.texMode == 1) return samplePSX8(tc);
	return samplePSX16(tc);
}

const vec2 c_LUTTexel = vec2(1.0 / 256.0, 1.0 / 256.0);
vec4 lut(vec2 rg) { return texture(s_rgLut, rg - c_LUTTexel * 0.0001); }

const mat4 c_dither = mat4(
	-4.0, +0.0, -3.0, +1.0,
	+2.0, -2.0, +3.0, -1.0,
	-3.0, +1.0, -4.0, +0.0,
	+3.0, -1.0, +2.0, -2.0) / 255.0;
vec4 dither(vec4 color) {
	ivec2 dc = ivec2(fract(gl_FragCoord.xy / 4.0) * 4.0);
	color.xyz += vec3(c_dither[dc.x][dc.y] * v_texcoord.w);
	return color;
}

bool dropsTexel() { if (pc.cutout == 2) return false; return pc.pass != 1 || pc.cutout == 1; }   // cutout 2: debug, nothing is dropped

vec4 bilinearTextureSample(vec2 P) {
	vec2 frac = fract(P);
	vec2 pixel = floor(P);
	vec2 C11 = samplePSX(pixel);
	vec2 C21 = samplePSX(pixel + vec2(1.0, 0.0));
	vec2 C12 = samplePSX(pixel + vec2(0.0, 1.0));
	vec2 C22 = samplePSX(pixel + vec2(1.0, 1.0));
	float ax1 = mix(float(C11.r + C11.g > 0.0), float(C21.r + C21.g > 0.0), frac.x);
	float ax2 = mix(float(C12.r + C12.g > 0.0), float(C22.r + C22.g > 0.0), frac.x);
	float axm = mix(ax1, ax2, frac.y);
	if (axm < 0.5 && dropsTexel()) discard;
	vec4 x1 = mix(lut(C11), lut(C21), frac.x);
	vec4 x2 = mix(lut(C12), lut(C22), frac.x);
	vec4 t = mix(x1, x2, frac.y);
	t.w = 1.0 - t.w;
	return t;
}
vec4 nearestTextureSample(vec2 P) {
	vec2 rg = samplePSX(P);
	if (rg.x + rg.y == 0.0 && dropsTexel()) discard;
	vec4 t = lut(rg);
	t.w = 1.0 - t.w;
	return t;
}

void main()
{
	if (pc.pass == 4 && v_shadowPos.y > intBitsToFloat(pc.cascade)) discard;   // below the water plane: not mirrored
	vec4 color;
	int hdKey = int(v_hdKey + 0.5);
	vec4 hole = fx.hdHole[hdKey];
	bool inHole = v_texcoord.x >= hole.x && v_texcoord.x < hole.z && v_texcoord.y >= hole.y && v_texcoord.y < hole.w;
	if (pc.texMode == 5) {
		// far field: page and palette come straight from the vertex, not from VRAM
		int layer = int(v_page_clut.x + 0.5);
		int pal = int(v_page_clut.y + 0.5);
		ivec2 t = ivec2(clamp(int(floor(v_texcoord.x)), 0, 255), clamp(int(floor(v_texcoord.y)), 0, 255));
		int index = int(texelFetch(s_pageIdx, ivec3(t, layer), 0).r * 255.0 + 0.5);
		vec4 c = texelFetch(s_pagePal, ivec2(index, layer * 64 + pal), 0);
		if (c.rgb == vec3(0.0) && c.a == 0.0 && (dropsTexel() || v_texcoord.w > 0.5)) discard;   // the PSX's transparent texel
		c.w = 1.0 - c.w;
		color = c;
	} else if (pc.texMode == 6) {
		// The water plane: almost clear. Under it an analytic bed (stone slabs, a net of light drifting on them) seen through the
		// surface with refraction, so that it moves with the view; a faint cyan-green tint that grows with the way through; the
		// mirrored city and sky on top by Fresnel. Everything sits on a world-fixed grid (periods that divide the plane's 4096
		// snap) and fades out when a pixel covers a good part of a period, so distance gives a smooth sheet.
		vec3 W = v_shadowPos.xyz;                     // camera-relative world position (the game's y points down)
		float dist = length(W);
		vec3 Vd = W / max(dist, 1.0);
		float tm = float(pc.bilinear) * (1.0 / 60.0);
		vec2 p = v_texcoord.xy;
		vec2 fw = fwidth(p);
		float foot = max(max(fw.x, fw.y), 0.0001);

		// gentle swell for the normal
		const vec2 dirs[3] = vec2[3](vec2(1.0, 0.25), vec2(-0.35, 1.0), vec2(0.8, -0.65));
		const float lens[3] = float[3](2048.0, 1024.0, 512.0);
		const float slopes[3] = float[3](0.008, 0.011, 0.009);
		vec2 g = vec2(0.0);
		for (int i = 0; i < 3; i++) {
			vec2 d = normalize(dirs[i]);
			float ph = dot(d, p) * (6.2831853 / lens[i]) + tm * (0.7 + 0.3 * float(i)) + float(i) * 1.7;
			g += d * slopes[i] * cos(ph) * clamp(1.0 - foot / (lens[i] * 0.34), 0.0, 1.0);
		}
		vec3 N = normalize(vec3(-g.x, -1.0, -g.y));   // up is -y
		float ndv = clamp(dot(-Vd, N), 0.0, 1.0);
		vec3 Rf = reflect(Vd, N);
		float F = clamp(0.02 + 0.95 * pow(1.0 - ndv, 4.0), 0.0, 1.0);

		// the bed, 520 units down, where the refracted ray lands
		vec3 Vr = refract(Vd, N, 1.0 / 1.33);
		float bedDepth = 520.0;
		float vy = max(Vr.y, 0.07);
		vec2 bp = p + Vr.xz * (bedDepth / vy);
		float bf = clamp(1.0 - foot * 2.5 / 400.0, 0.0, 1.0);
		vec2 cell = bp / 1024.0;
		vec2 fr = abs(fract(cell) - 0.5) * 2.0;
		float mortar = smoothstep(0.95, 1.0, max(fr.x, fr.y)) * bf;
		float spk = fract(sin(dot(floor(bp / 128.0), vec2(12.9898, 78.233))) * 43758.5453);
		vec3 bed = vec3(0.34, 0.32, 0.27) * (0.88 + 0.22 * spk * bf) * (1.0 - 0.30 * mortar);
		// light nets on the bed
		float net = 0.0;
		{
			float f1 = clamp(1.0 - foot / (512.0 * 0.30), 0.0, 1.0);
			float a1 = sin(dot(vec2(0.8, 0.6), bp) * (6.2831853 / 512.0) + tm * 0.55);
			float b1 = sin(dot(vec2(-0.6, 0.8), bp) * (6.2831853 / 512.0) - tm * 0.45);
			net += pow(clamp(1.0 - abs(a1 + b1) * 0.5, 0.0, 1.0), 7.0) * f1;
			float f2 = clamp(1.0 - foot / (256.0 * 0.30), 0.0, 1.0);
			float a2 = sin(dot(vec2(-0.9, 0.4), bp) * (6.2831853 / 256.0) - tm * 0.8);
			float b2 = sin(dot(vec2(0.3, 0.95), bp) * (6.2831853 / 256.0) + tm * 0.7);
			net += pow(clamp(1.0 - abs(a2 + b2) * 0.5, 0.0, 1.0), 7.0) * f2 * 0.8;
		}
		bed *= 1.0 + net * 0.55;

		// almost clear: a slight cyan-green that builds with the way through
		float path = bedDepth / vy;
		vec3 absorb = vec3(0.00070, 0.00032, 0.00042);
		vec3 tr = exp(-absorb * path);
		vec3 tint = vec3(0.060, 0.150, 0.150);
		vec3 wc = bed * tr + tint * (1.0 - tr);

		// the mirror
		vec3 hor = vec3(fx.fogR, fx.fogG, fx.fogB);
		vec3 skyc = mix(hor, hor * vec3(0.62, 0.78, 1.10), pow(clamp(-Rf.y, 0.0, 1.0), 0.55));
		vec3 Nv = N * fx.viewToWorld;
		vec2 suv = gl_FragCoord.xy / fx.screenInfo.xy + vec2(Nv.x, Nv.y) * 0.03;
		if (fx.screenInfo.z > 0.5) {
			vec4 rc = texture(uRefl, clamp(suv, vec2(0.002), vec2(0.998)));
			vec3 refl = mix(skyc, rc.rgb, rc.a);
			wc = mix(wc, refl, clamp(0.10 + F * 0.80, 0.0, 0.9));
		} else {
			// no mirror: just the faintest sky at a grazing view
			wc = mix(wc, skyc, F * 0.12);
		}

		// the sun's glint
		vec3 L = (fx.shadowOn != 0) ? normalize(vec3(fx.lightVP[0][0][2], fx.lightVP[0][1][2], fx.lightVP[0][2][2])) : normalize(vec3(0.35, -0.8, 0.45));
		if (L.y > 0.0) L = -L;
		vec3 Hh = normalize(L - Vd);
		float glint = pow(max(dot(N, Hh), 0.0), 260.0) * 9.0 + pow(max(dot(N, Hh), 0.0), 40.0) * 0.45;
		wc += vec3(1.0, 0.93, 0.8) * glint * clamp(1.0 - dist / 60000.0, 0.0, 1.0) * (0.4 + 0.6 * F);
		color = vec4(wc, 1.0);
	} else if (pc.texMode == 4) {
		color = texture(s_texture, v_texcoord.xy);
	} else if (pc.texMode < 3 && pc.pass != 1 && ((fx.hdMask >> hdKey) & 1) != 0 && !inHole) {
		vec4 hd = texture(uHDTex, vec3(v_texcoord.xy / 256.0, float(hdKey)));
		if (hd.a < 0.5) discard;
		color = vec4(hd.rgb, 1.0);
	} else if (pc.texMode == 3) {
		vec2 tc = v_texcoord.xy * pc.texelSize + pc.texelSize * 0.5;
		color = texture(s_texture, tc);
	} else {
		color = (pc.bilinear > 0) ? bilinearTextureSample(v_texcoord.xy) : nearestTextureSample(v_texcoord.xy);
	}
	if (pc.pass == 1 || pc.pass == 3) { fragColor = vec4(0.0); return; }
	vec4 outc = (pc.texMode == 4) ? vec4(color.rgb, 1.0) : dither(color * v_color);

	if (pc.pass == 2 && pc.texMode != 6 && v_is3D > 0.5 && fx.shadowOn != 0) {
		float lit = 1.0;
		int casc = -1;
		vec3 sc = vec3(0.0);
		for (int c = 0; c < 3; c++) {
			vec4 lp = fx.lightVP[c] * v_shadowPos;
			vec3 t = lp.xyz * 0.5 + 0.5;
			if (t.x > 0.02 && t.x < 0.98 && t.y > 0.02 && t.y < 0.98 && t.z > 0.0 && t.z < 1.0) { sc = t; casc = c; break; }
		}
		if (casc >= 0) {
			float texel = 1.0 / fx.shadowSize;
			float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
			float ca = cos(ign * 6.2831853), sa = sin(ign * 6.2831853);
			const vec2 pd[12] = vec2[12](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621), vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893), vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));
			int taps = (casc == 0) ? 12 : ((casc == 1) ? 6 : 4);
			float sum = 0.0;
			for (int i = 0; i < 12; i++) {
				if (i >= taps) break;
				vec2 o = vec2(pd[i].x * ca - pd[i].y * sa, pd[i].x * sa + pd[i].y * ca) * (2.2 * texel);
				sum += texture(uShadowMap, vec4(sc.xy + o, float(casc), sc.z - fx.shadowBias));
			}
			lit = sum / float(taps);
		}
		outc.rgb *= mix(fx.shadowStrength, 1.0, lit);
		if (fx.shadowDebug == 1) outc.rgb = (casc == 0) ? vec3(1.0, 0.0, 0.0) : ((casc == 1) ? vec3(0.0, 1.0, 0.0) : ((casc == 2) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 1.0)));
		if (fx.shadowDebug == 2) outc.rgb = vec3(lit);
	}

	if ((fx.lightN > 0 || fx.wet > 0.0) && pc.pass != 1 && pc.pass != 4 && v_is3D > 0.5) {
		vec3 Wp = v_shadowPos.xyz;
		vec3 V = normalize(-Wp);
		vec3 N = normalize(cross(dFdx(Wp), dFdy(Wp)));
		if (dot(N, -Wp) < 0.0) N = -N;
		vec3 acc = vec3(0.0);
		vec3 spec = vec3(0.0);
		for (int i = 0; i < 16; i++) {
			if (i >= fx.lightN) break;
			vec3 Lv = fx.lightA[i].xyz - Wp;
			float d = length(Lv);
			float rad = fx.lightA[i].w;
			if (d < rad) {
				vec3 Ld = Lv / d;
				float att = 1.0 - d / rad;
				att *= att;
				float ndl = max(dot(N, Ld), 0.0) * 0.8 + 0.2;
				float spot = 1.0;
				if (fx.lightC[i].w > 0.5) spot = smoothstep(fx.lightB[i].w, fx.lightB[i].w + 0.12, dot(-Ld, fx.lightC[i].xyz));
				float sh = 1.0;
				if (fx.lightC[i].w > 1.5 && spot > 0.0) {
					int k = int(fx.lightC[i].w - 1.5);
					vec4 slp = fx.spotVP[k] * vec4(Wp, 1.0);
					vec3 st = slp.xyz / slp.w * 0.5 + 0.5;
					if (slp.w > 0.0 && st.x > 0.0 && st.x < 1.0 && st.y > 0.0 && st.y < 1.0 && st.z < 1.0) {
						float tx = 1.0 / 1024.0;
						float ssum = 0.0;
						for (int dx = -1; dx <= 1; dx++)
							for (int dy = -1; dy <= 1; dy++)
								ssum += texture(uSpotMap, vec4(st.xy + vec2(float(dx), float(dy)) * tx, float(k), st.z - 0.0015));
						sh = ssum / 9.0;
					}
				}
				acc += fx.lightB[i].rgb * (att * ndl * spot * sh);
				vec3 Hh = normalize(Ld + V);
				spec += fx.lightB[i].rgb * (att * spot * sh * pow(max(dot(N, Hh), 0.0), 90.0));
			}
		}
		float wetK = (N.y < -0.8) ? fx.wet : 0.0;
		outc.rgb *= 1.0 - 0.3 * wetK;
		outc.rgb += color.rgb * acc * fx.lightStrength;
		outc.rgb += spec * (wetK * fx.lightStrength * 3.0);
		float fr = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0) * wetK;
		outc.rgb += vec3(fx.fogR, fx.fogG, fx.fogB) * (fr * 0.7);
	}

	if (fx.fogOn == 1 && pc.pass != 1 && v_is3D > 0.5 && v_fogDepth < 2000.0) {
		float fogF = clamp((v_fogDepth - fx.fogStart) / (fx.fogEnd - fx.fogStart), 0.0, 1.0);
		outc.rgb = mix(outc.rgb, vec3(fx.fogR, fx.fogG, fx.fogB), fogF);
	}
	if (pc.pass == 4) {
		// the mirrored far field: only what is above the plane, opaque
		outc.a = 1.0;
	}
	fragColor = outc;
}
