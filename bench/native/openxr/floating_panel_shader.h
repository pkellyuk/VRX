#pragma once

inline constexpr char kFloatingPanelHlsl[] = R"HLSL(
float4 PanelTexel(float2 uv, uint tile)
{
    if (any(uv<0) || any(uv>1)) return 0;
    const float2 tileSize=float2(1920,1536);
    float2 offset=float2(tile%2,tile/2)*tileSize;
    float2 atlasUV=(offset+clamp(uv*tileSize,.5,tileSize-.5))/(tileSize*float2(2,4));
    float4 colour=panelAtlas.SampleLevel(samp,atlasUV,0);
    return float4(Dec(saturate(colour.rgb/max(colour.a,.0001)))*colour.a,colour.a);
}

bool PanelCasing(float3 eye, float3 ray, float2 halfSize, float maxDistance, out float3 colour)
{
    float side=eye.z>=0 ? 1 : -1;
    float3 q=eye+float3(0,0,.004*side);
    float3 halfExtent=float3(halfSize*float2(1-32.0/960,1-32.0/768),.004);
    float3 inv=(step(0,ray)*2-1)/max(abs(ray),.000001);
    float3 a=(-halfExtent-q)*inv, b=(halfExtent-q)*inv;
    float3 front=min(a,b), back=max(a,b);
    float entry=max(front.x,max(front.y,front.z));
    float exit=min(back.x,min(back.y,back.z));
    colour=front.x>front.y ? float3(.075,.107,.129) : float3(.105,.15,.18);
    // The casing has open front and back faces. Only its thin rim is opaque.
    return exit>=entry && entry>0 && entry<maxDistance && max(front.x,front.y)>front.z;
}

float3 ComposePanels(float3 colour, float3 eye, float3 ray, float3 dx, float3 dy, float companionDepth)
{
    float maxDistance=companionDepth>0 ? companionDepth+.008 : 1000;
    float3 result=Dec(saturate(colour));
    [loop] for (uint i=0; i<vrexPanelCount; ++i)
    {
        float4 center=vrexPanels[i*3];
        float4 right=vrexPanels[i*3+1], up=vrexPanels[i*3+2];
        float3 normal=cross(right.xyz,up.xyz);
        float denominator=dot(normal,ray);
        if (abs(denominator)<.0001) continue;
        float3 fromCenter=eye-center.xyz;
        float distance=-dot(normal,fromCenter)/denominator;
        if (distance<=0 || distance>=maxDistance) continue;
        float3 relative=fromCenter+ray*distance;
        float2 scale=.5/float2(right.w,-up.w);
        float2 uv=.5+float2(dot(relative,right.xyz),dot(relative,up.xyz))*scale;
        if (any(abs(uv-.5)>.6)) continue;
        float3 casing;
        if (PanelCasing(float3(dot(fromCenter,right.xyz),dot(fromCenter,up.xyz),dot(fromCenter,normal)),
            float3(dot(ray,right.xyz),dot(ray,up.xyz),denominator),float2(right.w,up.w),maxDistance,casing))
            result=Dec(casing);
        float4 panel;
        if (smoothPanels != 0)
        {
            float3 x=distance*(dx-ray*dot(normal,dx)/denominator);
            float3 y=distance*(dy-ray*dot(normal,dy)/denominator);
            float2 u=float2(dot(x,right.xyz),dot(x,up.xyz))*scale*.25;
            float2 v=float2(dot(y,right.xyz),dot(y,up.xyz))*scale*.25;
            panel=(PanelTexel(uv-u-v,(uint)center.w)+PanelTexel(uv+u-v,(uint)center.w)+
                PanelTexel(uv-u+v,(uint)center.w)+PanelTexel(uv+u+v,(uint)center.w))*.25;
        }
        else panel=PanelTexel(uv,(uint)center.w);
        result=panel.rgb+result*(1-panel.a);
    }
    return Enc(result);
}
)HLSL";
