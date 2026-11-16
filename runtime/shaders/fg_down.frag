#version 450
// frame generation 2/4: next pyramid level (half size, 2x2 box through bilinear filtering). a.xy = target size
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } pc;
layout(location = 0) out vec4 o;
void main() { o = texture(src, gl_FragCoord.xy / pc.a.xy); }
