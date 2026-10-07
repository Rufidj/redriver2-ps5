// The frame's parameters: the game's two projections and every effect's settings, one slot per change
// (ps5/src/red2_render.cpp's FxUBO has the same layout).
layout(set = 1, binding = 0) uniform Fx {
	mat4 Projection;
	mat4 Projection3D;
	mat3 viewToWorld;        // camera-relative world from the game's view space
	mat4 lightVP[3];         // sun cascades
	mat4 spotVP[3];          // headlight shadow maps
	vec4 lightA[16];         // xyz position (camera-relative world), w radius
	vec4 lightB[16];         // rgb colour, w cone cosine
	vec4 lightC[16];         // xyz direction, w kind (0 point, 1 spot, 2+k spot with shadow map k)
	vec4 hdHole[32];
	float dispW, dispH, shadowSize, shadowStrength;
	float shadowBias, wet, lightStrength, fogR;
	float fogG, fogB, fogStart, fogEnd;
	int lightN, fogOn, hdMask, shadowDebug;
	int shadowOn;
	float skyH, farOfsX, farOfsY;   // skyH: the GTE projection plane distance (view rays, far field); far ofs: the GTE screen offset
	uvec4 farCol[9];                // the far field's colours: 0..31 lit walls, 32 combo, 33 ground
	vec4 screenInfo;                // render target width and height, then spare
	vec4 camInfo;                   // camera world x, z, time in seconds, windscreen drops
} fx;

layout(push_constant) uniform PC {
	int texMode;      // 0: 4 bit, 1: 8 bit, 2: 16 bit, 3: RGBA, 4: FMV
	int bilinear;
	vec2 texelSize;
	int pass;         // 0 plain, 1 shadow depth, 2 scene with shadows, 3 depth prepass
	int cascade;      // pass 1: 0..2 the sun, 3..5 a headlight
	int cutout;       // pass 1: alpha-tested casters (sprites)
	int pad;
} pc;
