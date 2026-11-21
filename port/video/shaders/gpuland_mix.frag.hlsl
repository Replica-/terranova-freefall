/* gpuland_mix.frag.hlsl -- the GPU ground (port/video/gpuland.c) put into the view's picture: a
 * custom render state of SDL's GPU renderer for drawing the picture's texture (platform.c).
 * Where the display wrote the ground's key colour (PORT_GL_KEY, 1 2 3) the card's ground is
 * taken instead, the texel's alpha (the 3D's mark for the anti-aliasing) kept; elsewhere the
 * picture as SDL would draw it.  The picture is drawn 1:1 or with nearest texels, so the key
 * comes through exactly.  Layout as common.hlsli: SDL's texture in space 2 slot 0, the extra
 * sampler (the ground) slot 1, the uniforms in space 3. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(combinedImageSampler) VK(binding(0, 2))
Texture2D u_texture : register(t0, space2);
VK(combinedImageSampler) VK(binding(0, 2))
SamplerState u_sampler : register(s0, space2);
VK(combinedImageSampler) VK(binding(1, 2))
Texture2D ground : register(t1, space2);
VK(combinedImageSampler) VK(binding(1, 2))
SamplerState ground_sampler : register(s1, space2);

VK(binding(0, 3))
cbuffer Map : register(b0, space3) {
    float4 map;     /* the ground's texel: (u * map.x, v * map.y + map.z) */
};

struct PSInput {
    VK(location(0)) float4 v_color : TEXCOORD0;
    VK(location(1)) float2 v_uv : TEXCOORD1;
};

float4 main(PSInput i) : SV_Target
{
    float4 c = u_texture.Sample(u_sampler, i.v_uv);
    float3 k = abs(c.rgb * 255.0 - float3(1.0, 2.0, 3.0));
    if (max(k.r, max(k.g, k.b)) < 0.5)
        c.rgb = ground.Sample(ground_sampler, float2(i.v_uv.x * map.x, i.v_uv.y * map.y + map.z)).rgb;
    return c * i.v_color;
}
