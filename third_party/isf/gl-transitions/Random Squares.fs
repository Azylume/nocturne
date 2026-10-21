/*{
    "CATEGORIES": [
        "Dissolve"
    ],
    "CREDIT": "Automatically converted from https://www.github.com/gl-transitions/gl-transitions/tree/master/randomsquares.glsl",
    "DESCRIPTION": "",
    "INPUTS": [
        {
            "NAME": "startImage",
            "TYPE": "image"
        },
        {
            "NAME": "endImage",
            "TYPE": "image"
        },
        {
            "DEFAULT": 0,
            "MAX": 1,
            "MIN": 0,
            "NAME": "progress",
            "TYPE": "float"
        },
        {
            "DEFAULT": 0.5,
            "MAX": 1,
            "MIN": 0,
            "NAME": "smoothness",
            "TYPE": "float"
        },
        {
            "DEFAULT": [
                10,
                10
            ],
            "MAX": [
                100,
                100
            ],
            "MIN": [
                0,
                0
            ],
            "NAME": "size",
            "TYPE": "point2D"
        }
    ],
    "ISFVSN": "2"
}
*/



vec4 getFromColor(vec2 inUV)	{
	return IMG_NORM_PIXEL(startImage, inUV);
}
vec4 getToColor(vec2 inUV)	{
	return IMG_NORM_PIXEL(endImage, inUV);
}



// Author: gre
// License: MIT

 
//	Integer PCG hash (Jarzynski and Olano 2020) of a whole cell coordinate, in place of the usual
//	fract(sin(x) * 43758.5453), whose result differs between GPUs.
uvec2 pcg2d(vec2 cell, uint salt)	{
	uvec2 v = uvec2(ivec2(cell)) * 1664525u + 1013904223u + salt;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	return v;
}

float rand (vec2 co) {
  return float(pcg2d(co, 0u).x >> 8u) / 16777216.0;
}

vec4 transition(vec2 p) {
  float r = rand(floor(vec2(size) * p));
  float m = smoothstep(0.0, -smoothness, r - (progress * (1.0 + smoothness)));
  return mix(getFromColor(p), getToColor(p), m);
}



void main()	{
	gl_FragColor = transition(isf_FragNormCoord.xy);
}