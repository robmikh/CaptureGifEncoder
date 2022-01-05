struct DiffRect
{
    uint left;
    uint top;
    uint right;
    uint bottom;
};

RWStructuredBuffer<DiffRect> diffBuffer : register(u0);
Texture2D<unorm float4> currentTexture : register(t0);
Texture2D<unorm float4> previousTexture : register(t1);

[numthreads(2, 2, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 position = DTid.xy;

    float4 currentColor = currentTexture[position];
    float4 previousColor = previousTexture[position];

    if ((currentColor.x != previousColor.x) || (currentColor.y != previousColor.y) || (currentColor.z != previousColor.z) || (currentColor.w != previousColor.w))
    {
        uint value = 0;
        InterlockedMin(diffBuffer[0].left, position.x, value);
        InterlockedMin(diffBuffer[0].top, position.y, value);
        InterlockedMax(diffBuffer[0].right, position.x, value);
        InterlockedMax(diffBuffer[0].bottom, position.y, value);
    }
}