#version 450
// Present: the game's picture onto the display, filtered; with the PS5 look's bloom and ambient occlusion
// (mode 1 builds the bloom texture: the bright parts of the picture, averaged).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D s_texture;
layout(set = 0, binding = 1) uniform sampler2D s_depth;
layout(set = 0, binding = 2) uniform sampler2D s_bloom;
layout(push_constant) uniform PC {
	float uBloom;
	float uBloomThr;
	float uAO;
	int uMode;
	vec2 uPixel;
	vec2 uBloomPixel;
	float uRain;
	float uTime;
} pc;

float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

// drops on the windscreen: two layers of round drops that grow, slide down a little and let the picture through bent
vec2 glassDrops(vec2 uv, float aspect, float t, out float dark)
{
	vec2 off = vec2(0.0);
	dark = 0.0;
	for (int L = 0; L < 3; L++) {
		float sc = (L == 0) ? 7.0 : ((L == 1) ? 12.0 : 20.0);
		vec2 q = uv * vec2(aspect, 1.0) * sc;
		vec2 id = floor(q);
		vec2 fq = fract(q) - 0.5;
		float h = hash21(id + float(L) * 17.3);
		float present = step(0.52, h);
		vec2 c = (vec2(hash21(id + 3.1), hash21(id + 7.7)) - 0.5) * 0.35;
		float life = fract(t * (0.05 + 0.04 * h) + h * 5.0);
		c.y += life * life * 0.30;
		vec2 d = fq - c;
		d.y *= 0.8;
		float rad = (0.13 + 0.06 * hash21(id + 11.0)) * mix(0.6, 1.0, smoothstep(0.0, 0.25, life)) * (1.0 - 0.3 * smoothstep(0.85, 1.0, life));
		float r = length(d);
		float m = smoothstep(rad, rad * 0.55, r) * present;
		off += -d / max(r, 0.001) * m * (r / rad) * (0.030 / sc * 7.0);
		dark += m * 0.07;
	}
	return off;
}

float linZ(float d) { return 2.0 * 6000.0 * 0.25 / (6000.25 - (2.0 * d - 1.0) * 5999.75); }

void main()
{
	if (pc.uMode == 1) {
		vec3 acc = vec3(0.0);
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++) {
				vec2 o = (vec2(float(x), float(y)) - 1.5) * 2.0 * pc.uPixel;
				acc += max(textureLod(s_texture, uv + o, 0.0).rgb - pc.uBloomThr, 0.0);
			}
		fragColor = vec4(acc / 16.0, 1.0);
		return;
	}
	vec2 suv = uv;
	float glassDark = 0.0;
	if (pc.uRain > 0.0) {
		float aspect = pc.uPixel.y / pc.uPixel.x;
		suv = uv + glassDrops(uv, aspect, pc.uTime, glassDark) * pc.uRain;
	}
	vec3 col = textureLod(s_texture, suv, 0.0).rgb * (1.0 - glassDark * pc.uRain);
	if (pc.uAO > 0.0) {
		float d0 = texture(s_depth, uv).r;
		if (d0 < 0.99999) {
			float z0 = linZ(d0);
			float rw = 1.2;
			float rpx = clamp(rw * 1.0 / z0 * 0.5 / pc.uPixel.y * 0.4, 2.0, 40.0);
			float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
			float occ = 0.0;
			for (int i = 0; i < 12; i++) {
				float a = (float(i) + ign) * 2.3999632;
				float r = (float(i) + 0.5) / 12.0;
				vec2 o = vec2(cos(a), sin(a)) * (r * rpx) * pc.uPixel;
				float ds = texture(s_depth, uv + o).r;
				float dz = z0 - linZ(ds);
				occ += clamp(dz / (0.05 + 0.02 * z0), 0.0, 1.0) * (1.0 - clamp(dz / (2.5 * rw), 0.0, 1.0));
			}
			col *= 1.0 - pc.uAO * occ / 12.0;
		}
	}
	if (pc.uBloom > 0.0) {
		vec3 b = texture(s_bloom, uv).rgb * 0.30;
		b += (texture(s_bloom, uv + vec2(pc.uBloomPixel.x, 0.0) * 1.3).rgb + texture(s_bloom, uv - vec2(pc.uBloomPixel.x, 0.0) * 1.3).rgb
			+ texture(s_bloom, uv + vec2(0.0, pc.uBloomPixel.y) * 1.3).rgb + texture(s_bloom, uv - vec2(0.0, pc.uBloomPixel.y) * 1.3).rgb) * 0.10;
		b += (texture(s_bloom, uv + pc.uBloomPixel * 2.6).rgb + texture(s_bloom, uv - pc.uBloomPixel * 2.6).rgb
			+ texture(s_bloom, uv + vec2(pc.uBloomPixel.x, -pc.uBloomPixel.y) * 2.6).rgb + texture(s_bloom, uv + vec2(-pc.uBloomPixel.x, pc.uBloomPixel.y) * 2.6).rgb) * 0.075;
		col += b * (pc.uBloom * 2.0);
	}
	fragColor = vec4(col, 1.0);
}
