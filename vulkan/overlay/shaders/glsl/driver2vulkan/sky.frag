#version 450
#extension GL_GOOGLE_include_directive : require
// The HD sky: an equirectangular panorama looked up by the view ray of each pixel. The ray is rebuilt from the game's
// own projection (the GTE distance H and the 3D projection of the renderer), so the sky turns exactly like the world.
layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 fragColor;

#include "fx.glsl"

layout(set = 0, binding = 0) uniform sampler2D s_pano;

void main()
{
	// pc.texelSize.x: the panorama's turn (in revolutions) that puts its sun where the game's sun is;
	// pc.texelSize.y: the game's sky brightness
	vec3 v = vec3(ndc.x * fx.dispW / (fx.skyH * fx.Projection3D[0][0]),
		-ndc.y * fx.dispH / (fx.skyH * fx.Projection3D[1][1]), 1.0);
	vec3 d = normalize(fx.viewToWorld * v);
	float az = atan(d.x, d.z) * 0.15915494 + 0.5 + pc.texelSize.x;
	float el = asin(clamp(-d.y, -1.0, 1.0));
	vec2 uv = vec2(fract(az), clamp(0.5 - el * 0.31830989, 0.0, 1.0));
	vec3 c = texture(s_pano, uv).rgb;
	// below the horizon the panorama is only a reflection: let it sink into the haze
	if (el < 0.0)
		c *= mix(1.0, 0.7, clamp(-el * 4.0, 0.0, 1.0));
	fragColor = vec4(c * pc.texelSize.y, 1.0);
}
