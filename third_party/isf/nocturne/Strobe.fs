/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "On for duty of every rate beats of the beat clock, from each period's start; off is transparent",
    "NOCTURNE": {
        "TYPE": "strobe",
        "CATEGORY": "",
        "PARAMS": {
            "rate": {"DRIVE": "BEATS", "UNIT": {"SUFFIX": " beats", "SCALE": 1, "DECIMALS": 2}},
            "duty": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "red": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "green": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "blue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "rate", "TYPE": "float", "MIN": 0.25, "MAX": 16, "DEFAULT": 1},
        {"NAME": "duty", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "red", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "green", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "blue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1}
    ],
    "ISFVSN": "2"
}
*/

void main()
{
    gl_FragColor = fract(rate) < duty ? vec4(red, green, blue, 1.0) : vec4(0.0);
}
