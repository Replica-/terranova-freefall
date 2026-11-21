/* gpuwx.frag.hlsl -- GPU rendering's weather (port/video/gpuland.c, gpuwx.vert.hlsl): a rain streak
 * soft across its width and along its length, or a round snow flake soft at its edge, in the
 * weather's colour, premultiplied (the pass blends them over each other: one, one - alpha).
 * SDL_GPU's layout for fragment shaders: uniforms in space 3. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 3))
cbuffer Tint : register(b0, space3) {
    float4 colour;      /* rgb, the strength's scale */
    float4 kind;        /* 0 rain / 1 snow */
};

struct PSInput {
    float4 pos : SV_Position;
    VK(location(0)) float2 q : TEXCOORD0;
    VK(location(1)) float a : TEXCOORD1;
};

float4 main(PSInput i) : SV_Target0
{
    float a;
    if (kind.x < 0.5)
        a = (1.0 - i.q.x * i.q.x) * smoothstep(0.0, 0.35, i.q.y) * smoothstep(1.0, 0.6, i.q.y);
    else
        a = 1.0 - smoothstep(0.45, 1.0, length(i.q));
    a *= i.a * colour.a;
    return float4(colour.rgb * a, a);
}
