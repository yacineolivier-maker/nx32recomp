// Triangle couvrant l'écran (passe de conversion HDR10, cf. src/hdr.cpp)
#version 450

layout(location = 0) out vec2 texcoord;

void main() {
    const float x = float((gl_VertexIndex & 1) << 2);
    const float y = float((gl_VertexIndex & 2) << 1);
    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);
    texcoord = vec2(x, y) * 0.5;
}
