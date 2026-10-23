// Conversion de l'image SDR du jeu (sRGB, BT.709) en HDR10 (PQ ST.2084, BT.2020) pour une chaîne d'échange
// A2B10G10R10 / HDR10_ST2084 (cf. src/hdr.cpp). L'image du jeu est placée au blanc de référence choisi (nits) ;
// seules ses zones les plus lumineuses (reflets, ciel, effets) sont étendues vers la luminosité maximale de l'écran.
#version 450

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D image_sdr;

layout(push_constant) uniform Reglages {
    float blanc;      // nits du blanc SDR (203 = référence BT.2408)
    float crete;      // nits maximum de l'écran
    float saturation; // 1 = couleurs d'origine
    float seuil;      // luminance SDR (0..1) à partir de laquelle les reflets sont étendus
};

vec3 srgb_vers_lineaire(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// Encodage PQ (SMPTE ST 2084) : L en fraction de 10 000 nits
vec3 pq(vec3 L) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const vec3 p = pow(clamp(L, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    vec3 lin = srgb_vers_lineaire(clamp(texture(image_sdr, texcoord).rgb, 0.0, 1.0));
    const float y = dot(lin, vec3(0.2126, 0.7152, 0.0722));
    lin = max(mix(vec3(y), lin, saturation), 0.0);

    // extension des hautes lumières : identité sous le seuil, puis courbe douce (dérivée continue au seuil) jusqu'à
    // la crête pour le blanc pur ; appliquée à la luminance pour garder la teinte
    float gain = 1.0;
    const float max_lin = crete / blanc;
    if (y > seuil && max_lin > 1.0) {
        const float t = clamp((y - seuil) / (1.0 - seuil), 0.0, 1.0);
        const float etendue = seuil + (1.0 - seuil) * t + (max_lin - 1.0) * t * t * t;
        gain = etendue / y;
    }
    const vec3 nits = lin * gain * blanc;

    // primaires BT.709 -> BT.2020 (linéaire)
    const mat3 bt709_vers_bt2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                                        0.3292820, 0.9195400, 0.0880132,
                                        0.0433136, 0.0113612, 0.8955950);
    color = vec4(pq(bt709_vers_bt2020 * nits / 10000.0), 1.0);
}
