/* gpuwx.vert.hlsl -- GPU rendering's weather (port/video/gpuland.c): rain and snow as particles in
 * the world around the viewer, drawn over the 3D view (gpuland_mix puts them on its pixels).
 * Each instance is a drop or a flake: a fixed place in a box of the world tiled around the viewer
 * (bx squares across, bz high), moving with the wind and falling, so that it stays where it is in
 * the world as the view turns and walks.  Its place is turned into the game's view space as the
 * ground's corners are (gpufar.c: the view matrix on (ey, height, ex), a square 1.0 across, a
 * height unit 1/24 of a square), projected as the ground is (w = z, the ground's depth hides it):
 *   rain: a streak from where the drop is to where it was a moment ago, a few pixels wide;
 *   snow: a round flake facing the view.
 * No vertex buffer: six vertices an instance, the place from the instance's number.
 * SDL_GPU's layout for vertex shaders: uniform buffers in space 1. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 1))
cbuffer Weather : register(b0, space1) {
    float4 proj;        /* as gpuland.vert.hlsl: x_clip = proj.x * x + proj.y * z, y alike */
    float4 depth;       /* z_clip = depth.x * z + depth.y */
    float4 m0, m1, m2;  /* the view matrix's rows (on ey, height, ex) */
    float4 mz;          /* its offset */
    float4 cam;         /* the viewer: x, y on the map (squares), height (units), time (s) */
    float4 kind;        /* 0 rain / 1 snow, the box across, its height (squares), the pixel size */
    float4 move;        /* wind x, y, fall (squares a second), the streak's time (s) */
    float4 look;        /* the size (squares: snow), the width (pixels: rain), the canvas w, h */
};

struct VSOutput {
    float4 pos : SV_Position;
    VK(location(0)) float2 q : TEXCOORD0;      /* across the streak / flake: -1 .. 1 */
    VK(location(1)) float a : TEXCOORD1;       /* its strength (nearer: more) */
};

float hash(uint n)
{
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / float(0x7fffffff);
}

/* a place relative to the viewer (squares: x east, y south, z up) in view space */
float3 to_view(float3 r)
{
    float3 w = float3(r.y, cam.z + r.z * 24.0, r.x);    /* (ey, height units, ex) */
    return float3(dot(m0.xyz, w), dot(m1.xyz, w), dot(m2.xyz, w)) + mz.xyz;
}

float4 clip_of(float3 v)
{
    return float4(proj.x * v.x + proj.y * v.z, proj.z * v.y + proj.w * v.z, depth.x * v.z + depth.y, v.z);
}

VSOutput main(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    static const float2 corner[6] = { float2(-1, 0), float2(1, 0), float2(1, 1), float2(-1, 0), float2(1, 1), float2(-1, 1) };
    VSOutput o;
    float2 c = corner[vid];
    float bx = kind.y, bz = kind.z, t = cam.w;
    float3 base = float3(hash(inst * 3u + 1u), hash(inst * 3u + 2u), hash(inst * 3u + 3u));
    float3 vel = float3(move.x, move.y, -move.z) * (0.75 + 0.5 * hash(inst * 7u + 11u));
    float3 p = base * float3(bx, bx, bz) + vel * t;
    if (kind.x > 0.5) {                 /* snow: a sway around its fall */
        float ph = hash(inst * 5u + 4u) * 6.2832;
        p.xy += 0.35 * float2(sin(t * 0.9 + ph), cos(t * 0.7 + ph * 1.3));
    }
    /* the box tiled around the viewer: the instance's copy nearest to it */
    float3 r = p - float3(cam.x, cam.y, 0.0);
    r.xy = r.xy - bx * floor(r.xy / bx + 0.5);
    r.z = r.z - bz * floor(r.z / bz + 0.5);
    float3 v0 = to_view(r);
    float dist = length(r);
    o.a = saturate(1.0 - dist / (0.5 * bx)) * saturate((v0.z - 0.15) * 4.0);
    o.q = c * float2(1, 2) - float2(0, 1);
    if (kind.x < 0.5) {
        /* rain: from now back along the fall, widened across it on the screen */
        float3 v1 = to_view(r - vel * move.w);
        float4 c0 = clip_of(v0), c1 = clip_of(v1);
        if (c0.w < 0.05 || c1.w < 0.05) {
            o.pos = float4(0, 0, -1, 1);    /* (behind the viewer: off) */
            return o;
        }
        float2 s0 = c0.xy / c0.w * look.zw * 0.5, s1 = c1.xy / c1.w * look.zw * 0.5;
        float2 d = s1 - s0;
        float len = max(length(d), 1e-3);
        float2 n = float2(-d.y, d.x) / len * look.y * 0.5;
        float4 cc = c.y > 0.5 ? c1 : c0;
        cc.xy += n * c.x * 2.0 / look.zw * cc.w;
        o.pos = cc;
        o.q = float2(c.x, c.y);
    } else {
        /* snow: a square facing the view, a round flake in it */
        /* (not much bigger than 8 pixels however near: a flake by the eye is a blur, not a disc) */
        float sz = look.x * (0.6 + 0.8 * hash(inst * 13u + 5u));
        sz = min(sz, 8.0 / max(look.z * 0.5 * abs(proj.x), 1.0) * v0.z);
        float3 v = v0 + float3(c.x * sz, (c.y * 2.0 - 1.0) * sz, 0);
        if (v0.z < 0.05) {
            o.pos = float4(0, 0, -1, 1);
            return o;
        }
        o.pos = clip_of(v);
        o.q = float2(c.x, c.y * 2.0 - 1.0);
    }
    return o;
}
