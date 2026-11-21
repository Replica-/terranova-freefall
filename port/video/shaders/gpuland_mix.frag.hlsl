/* gpuland_mix.frag.hlsl -- the GPU ground (port/video/gpuland.c) put into the view's picture: a
 * custom render state of SDL's GPU renderer for drawing the picture's texture (platform.c).
 * The display gave the pixels that show the ground the alpha PORT_GL_MARK_A (253), their colour
 * kept: there the card's ground is taken where the card drew any (its alpha 1), the picture's own
 * colour where it drew none (a silhouette the two draw a little differently), and the pixel gets
 * the 3D's alpha (map.w: 254 for the anti-aliasing's mark, or 1); elsewhere the picture as SDL
 * would draw it.  The picture is drawn 1:1 or with nearest texels, so the mark comes through
 * exactly.  Layout as common.hlsli: SDL's texture in space 2 slot 0, the extra sampler (the
 * ground) slot 1, the uniforms in space 3. */
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
    float4 map;     /* the ground's texel: (u * map.x, v * map.y + map.z); map.w the 3D's alpha */
};

struct PSInput {
    VK(location(0)) float4 v_color : TEXCOORD0;
    VK(location(1)) float2 v_uv : TEXCOORD1;
};

float4 main(PSInput i) : SV_Target
{
    float4 c = u_texture.Sample(u_sampler, i.v_uv);
    if (abs(c.a * 255.0 - 253.0) < 0.5) {
        float4 g = ground.Sample(ground_sampler, float2(i.v_uv.x * map.x, i.v_uv.y * map.y + map.z));
        if (g.a > 0.5)
            c.rgb = g.rgb;
        c.a = map.w;
    }
    return c * i.v_color;
}
