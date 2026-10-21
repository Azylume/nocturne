/*{
    "CATEGORIES": [
        "Generator", "Geometry", "Noise"
    ],
    "CREDIT": "by VIDVOX",
    "DESCRIPTION": "Worley/Voronoi cellular noise generator.",
    "ISFVSN": "2",
    "INPUTS": [
        {
            "DEFAULT": 10.0,
            "LABEL": "Density",
            "MAX": 50.0,
            "MIN": 1.0,
            "NAME": "density",
            "TYPE": "float"
        },
        {
            "DEFAULT": 1.0,
            "LABEL": "Jitter",
            "MAX": 1.0,
            "MIN": 0.0,
            "NAME": "jitter",
            "TYPE": "float"
        },
        {
            "DEFAULT": 0.0,
            "LABEL": "Speed",
            "MAX": 2.0,
            "MIN": 0.0,
            "NAME": "speed",
            "TYPE": "float"
        },
        {
            "DEFAULT": 0,
            "LABEL": "Distance Metric",
            "LABELS": [
                "Euclidean (Round)",
                "Manhattan (Diamond)",
                "Chebyshev (Square)"
            ],
            "NAME": "metric",
            "TYPE": "long",
            "VALUES": [
                0,
                1,
                2
            ]
        },
        {
            "DEFAULT": 0,
            "LABEL": "Color Mode",
            "LABELS": [
                "Random Per Cell",
                "Distance Gradient",
                "F1 Grayscale"
            ],
            "NAME": "colorMode",
            "TYPE": "long",
            "VALUES": [
                0,
                1,
                2
            ]
        },
        {
            "DEFAULT": [
                0.1,
                0.4,
                0.8,
                1.0
            ],
            "LABEL": "Color 1",
            "NAME": "cellColor1",
            "TYPE": "color"
        },
        {
            "DEFAULT": [
                0.9,
                0.7,
                0.2,
                1.0
            ],
            "LABEL": "Color 2",
            "NAME": "cellColor2",
            "TYPE": "color"
        },
        {
            "DEFAULT": 0.05,
            "LABEL": "Border Width",
            "MAX": 0.5,
            "MIN": 0.0,
            "NAME": "borderWidth",
            "TYPE": "float"
        },
        {
            "DEFAULT": [
                0.0,
                0.0,
                0.0,
                1.0
            ],
            "LABEL": "Border Color",
            "NAME": "borderColor",
            "TYPE": "color"
        },
        {
            "DEFAULT": 0.0,
            "LABEL": "Seed",
            "MAX": 100.0,
            "MIN": 0.0,
            "NAME": "seed",
            "TYPE": "float"
        }
    ]
}*/


#define TAU 6.28318530717958647692


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

//	2D hash returning (0..1, 0..1) per cell and seed.
vec2 hash2(vec2 p)	{
	return vec2(pcg2d(p, floatBitsToUint(seed)) >> 8u) / 16777216.0;
}

//	1D hash for per-cell color lookup.
float hash1(vec2 p)	{
	return float(pcg2d(p, floatBitsToUint(seed) + 17u).x >> 8u) / 16777216.0;
}

//	Distance under the selected metric.
//	0 = Euclidean (round cells)
//	1 = Manhattan (diamond cells)
//	2 = Chebyshev (square cells)
float distMetric(vec2 d)	{
	if (metric == 1) return abs(d.x) + abs(d.y);
	if (metric == 2) return max(abs(d.x), abs(d.y));
	return length(d);
}


void main()	{
	vec2 uv = isf_FragNormCoord;
	
	//	Cell-space coordinate, aspect-corrected so cells render visually square
	//	regardless of frame aspect.
	float aspect = RENDERSIZE.x / RENDERSIZE.y;
	vec2  cellSpace = vec2(uv.x * aspect, uv.y) * density;
	vec2  cell = floor(cellSpace);
	
	//	Track the closest (F1) and second-closest (F2) feature-point distances.
	//	Their difference (F2 - F1) is the standard Worley edge function.
	float F1 = 1e10;
	float F2 = 1e10;
	vec2  winnerCell = cell;
	
	//	3x3 neighborhood scan — sufficient with jitter clamped to <=1.
	for (int dy = -1; dy <= 1; ++dy)	{
		for (int dx = -1; dx <= 1; ++dx)	{
			vec2 neighbor = cell + vec2(float(dx), float(dy));
			vec2 h = hash2(neighbor);
	
			//	Deterministic jittered base position within the neighbor cell.
			vec2 basePos = vec2(0.5) + (h - 0.5) * jitter;
	
			//	Optional sinusoidal drift when speed > 0. Each feature point
			//	gets its own phase via the cell hash, so the field evolves
			//	continuously without any sudden re-shuffling.
			vec2 anim = speed * 0.3 * vec2(
				sin(TIME * speed + h.x * TAU),
				cos(TIME * speed + h.y * TAU)
			);
	
			vec2  fpPos = neighbor + basePos + anim;
			float d = distMetric(cellSpace - fpPos);
	
			if (d < F1)	{
				F2 = F1;
				F1 = d;
				winnerCell = neighbor;
			}
			else if (d < F2)	{
				F2 = d;
			}
		}
	}
	
	//	===== Cell fill =====
	vec3 fill;
	if (colorMode == 0)	{
		//	Random Per Cell — hash the winning cell's coordinate to pick a
		//	color mix factor between Color 1 and Color 2.
		float cellHash = hash1(winnerCell);
		fill = mix(cellColor1.rgb, cellColor2.rgb, cellHash);
	}
	else if (colorMode == 1)	{
		//	Distance Gradient — Color 1 at the feature point, Color 2 at the edge.
		float t = clamp(F1, 0.0, 1.0);
		fill = mix(cellColor1.rgb, cellColor2.rgb, t);
	}
	else	{
		//	F1 Grayscale — raw nearest-distance value, useful as a luminance source.
		fill = vec3(clamp(F1, 0.0, 1.0));
	}
	
	//	===== Cell border =====
	//	(F2 - F1) is the distance to the boundary between this cell and its
	//	nearest neighbor. Small = near border, large = deep in the cell.
	float edgeDist = F2 - F1;
	float borderAmount = 1.0 - smoothstep(borderWidth * 0.5, borderWidth, edgeDist);
	
	vec3 finalColor = mix(fill, borderColor.rgb, borderAmount * borderColor.a);
	
	gl_FragColor = vec4(finalColor, 1.0);
}
