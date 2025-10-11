#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

void main() {
    // Debug: Show normals as colors to verify geometry is correct
    vec3 normalColor = normalize(fragNormal) * 0.5 + 0.5;

    // Also show some simple lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 normal = normalize(fragNormal);
    float diffuse = max(dot(normal, lightDir), 0.0) * 0.8 + 0.2;

    vec3 color = normalColor * diffuse;
    outColor = vec4(color, 1.0);
}
