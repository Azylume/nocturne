/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Turns the picture's alpha into a mask from its colour, so black turns transparent; the colour stays as it is",
    "NOCTURNE": {"TYPE": "classicAutoMask", "CATEGORY": "Classic"},
    "INPUTS": [
        {
            "NAME": "inputImage",
            "TYPE": "image"
        },
        {
            "DEFAULT": 0,
            "LABEL": "Channel",
            "LABELS": ["Luminance", "Red", "Green", "Blue"],
            "NAME": "channel",
            "TYPE": "long",
            "VALUES": [0, 1, 2, 3]
        },
        {
            "DEFAULT": 0.5,
            "LABEL": "Contrast",
            "MAX": 1,
            "MIN": 0,
            "NAME": "contrast",
            "TYPE": "float"
        },
        {
            "DEFAULT": false,
            "LABEL": "Invert",
            "NAME": "invert",
            "TYPE": "bool"
        }
    ],
    "ISFVSN": "2"
}
*/

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    float m = channel == 0 ? dot(c.rgb, vec3(0.299, 0.587, 0.114))
            : channel == 1 ? c.g + c.b : channel == 2 ? c.r + c.b : c.r + c.g;
    float gain = contrast <= 0.5 ? 2.0 * contrast : 1.0 + 10.0 * (contrast - 0.5);
    gl_FragColor = vec4(c.rgb, c.a * clamp((invert ? 1.0 - m : m) * gain, 0.0, 1.0));
}
