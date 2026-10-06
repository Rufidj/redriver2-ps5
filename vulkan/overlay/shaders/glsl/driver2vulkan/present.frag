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
} pc;

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
	vec3 col = textureLod(s_texture, uv, 0.0).rgb;
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
