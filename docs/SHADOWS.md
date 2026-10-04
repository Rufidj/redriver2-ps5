# Real-time shadows

The original game draws flat blob shadows. The port renders a **shadow map from the sun** (or the moon at night) and darkens
what is hidden from it. It adds no GLSL program (the driver allows two): the uber shader gets a pass number.

## Frame flow

1. `DrawMapPSX` (Game/C/draw.c) resets the caster list and calls `GR_PS5_SetShadowParams` with the sun vector, the camera
   rotation and the camera position. The light is an orthographic projection looking along the sun, centred 2500 units in
   front of the camera, 10000 units wide and 40000 deep, snapped to the shadow-map texel grid in absolute world space so
   shadows do not swim when the camera moves.
2. While the frame's models are drawn, the drawing code also submits **whole models** as shadow casters:
   `PlotBuildingModel` and `PlotModelSubdivNxN` (buildings and street objects: every polygon), `plotNewCarModel`
   (every car triangle), `DrawSprites` (trees: textured quads turned to face the sun, alpha tested) and
   `DrawAllPedestrians` (an upright box per pedestrian).
3. In `DrawAllSplits` (PsyCross/src/gpu/PsyX_GPU.cpp), before the first big batch is drawn, the shadow pass renders the casters
   into a 4096x4096 depth texture (depth-only, polygon offset), then the normal pass samples it with a 3x3 PCF
   (`sampler2DShadow`) for every 3D vertex and darkens by `shadowStrength`.

## Why the casters are submitted by the drawing code

The first version re-used the triangles that reach the GPU. That cannot work here: the game culls back faces **in
software** (`gte_nclip`) before building primitives, so the sun only sees the faces turned towards the camera and the shadows had
holes (cars with missing parts, buildings without the sides that matter). Matching the GPU's vertices to their 3D data through
screen coordinates is also fragile. Submitting the complete model, transformed with the current GTE matrices but not culled,
fixes both. This is the same idea used by the author's Super Mario 64 port: the code that knows the model feeds the shadow map.

## Limits

* Only what the game draws that frame casts (the game culls objects outside the view cone, so a building just off screen does
  not cast into the view).
* Receivers are the vertices that have PGXP data (about 98 % of them); the rest are not shadowed.
* Car wheels and doors that are separate models, pedestrian bodies and debris are not (yet) exact casters.

## Debugging aids (`config.ini [render]`)

`shadowDebug=2` paints lit pixels white and shadowed ones black without textures; `1` shows the shadow-map coordinates;
`5` dumps statistics and an ASCII picture of the depth map to the log. `shadowFlip=1` reverses the light.
