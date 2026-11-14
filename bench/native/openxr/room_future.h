#pragma once

// Shares the scalar CPU/HLSL adapters from room_themes.h. Space is sampled from
// ray direction, so stars remain distant while the viewer moves past a viewport.
#define VRX_ROOM_FUTURE_CODE \
int FutureSurface(int env,int face,float a,float b,float floorY,float fa,float fb,float dx,float dy,float dz,float gain,ThemeRef(R),ThemeRef(G),ThemeRef(B)) { \
    bool ship=env==8, horizontal=face==3 || face==4; \
    float y=horizontal ? b:b-floorY, foot=ThemeMax(.001f,ThemeMax(fa,fb)); \
    float bay=ship ? 2.8f:2.4f, x=a-bay*ThemeFloor(a/bay+.5f); \
    float join=ThemeMax(ThemePulse(a,bay,.045f,fa),ThemePulse(y,1.2f,.025f,fb)); \
    float lip=ThemeMax(ThemePulse(a,bay,.10f,fa),ThemePulse(y,1.2f,.07f,fb)); \
    float brushed=1.0f+.045f*ThemeFolds(a,.025f,fa); \
    float panel=(1.0f-.8f*join+.22f*(lip-join))*brushed; \
    R*=panel; G*=panel; B*=panel; \
    float cyan=0.0f,pink=0.0f,amber=0.0f; int part=1; \
    if (horizontal) { \
        if (face==4) { \
            float frame=ThemeMax(ThemePulse(a,bay,.17f,fa),ThemePulse(b,1.8f,.13f,fb)); \
            R*=1.0f-.6f*frame; G*=1.0f-.6f*frame; B*=1.0f-.6f*frame; \
            cyan=ThemePulse(a,bay,.026f,fa); \
            float cross=ThemePulse(b,1.8f,.025f,fb); \
            if(ship) cyan=ThemeMax(cyan,cross*.65f); else pink=cross; \
            float vent=ThemeBar(x,0.0f,.75f,fa)*ThemePulse(b,.13f,.04f,fb)*(1.0f-frame); \
            R*=1.0f-.5f*vent; G*=1.0f-.5f*vent; B*=1.0f-.5f*vent; \
        } else { \
            float stripe=ThemePulse(ThemeAbs(a),2.8f,.035f,fa); cyan=stripe*.65f; \
            float grain=1.0f+.055f*ThemeFolds(a+b,.06f,fa+fb); R*=grain; G*=grain; B*=grain; \
            if(ship) amber=ThemePulse(a+b,.35f,.12f,fa+fb)*ThemeBar(ThemeAbs(a),3.9f,.24f,fa)*.4f; \
        } \
    } else { \
        float upright=ThemeBar(ThemeAbs(x),bay*.5f-.16f,.13f,fa); \
        R*=1.0f-.55f*upright; G*=1.0f-.55f*upright; B*=1.0f-.55f*upright; \
        cyan=ThemeBar(ThemeAbs(x),bay*.5f-.16f,.025f,fa)*ThemeBar(y,1.8f,3.1f,fb); \
        float rail=ThemeMax(ThemeBar(y,.23f,.025f,fb),ThemeBar(y,3.3f,.025f,fb)); \
        if(ship) cyan=ThemeMax(cyan,rail*.5f); else pink=rail; \
        float fixture=ThemeBar(x,0.0f,1.45f,fa)*ThemeBar(y,1.9f,.95f,fb); \
        float border=ThemeMax(0.0f,ThemeBar(x,0.0f,1.55f,fa)*ThemeBar(y,1.9f,1.05f,fb)-fixture); \
        if(ship) { \
            float radial=ThemeSqrt(x*x/(.74f*.74f)+(y-1.95f)*(y-1.95f)/(.61f*.61f)); \
            float aperture=ThemeSat((.88f-radial)*.61f/foot+.5f); \
            float rim=ThemeMax(0.0f,ThemeSat((1.0f-radial)*.61f/foot+.5f)-aperture); \
            float inner=ThemeBar(radial,.89f,.018f,foot/.61f); \
            float h=ThemeSqrt(dx*dx+dz*dz), az=h>.00001f ? ThemeAtan2(dx,dz):0.0f, el=ThemeAtan2(dy,ThemeMax(h,.00001f)); \
            float su=az*60.0f,sv=el*60.0f,si=ThemeFloor(su),sj=ThemeFloor(sv); \
            float sx=su-si-.2f-.6f*ThemeHash((int)si,(int)sj); \
            float sy=sv-sj-.2f-.6f*ThemeHash((int)si+211,(int)sj-71); \
            float starW=ThemeMax(.045f,ThemeMin(.3f,foot*8.0f)); \
            float star=ThemeExp(-(sx*sx+sy*sy)/(starW*starW))*.002025f/(starW*starW); \
            float cloud=ThemeExp(-(el+.12f*ThemeSin(az*2.0f))*(el+.12f*ThemeSin(az*2.0f))*48.0f); \
            float planetX=az-1.6f,planetY=el+.13f,planetD=ThemeSqrt(planetX*planetX+planetY*planetY); \
            float planet=ThemeSat((.21f-planetD)/ThemeMax(.0001f,foot*.15f)+.5f); \
            float bands=.6f+.4f*ThemeSin(planetY*65.0f+ThemeSin(planetX*18.0f)); \
            float light=ThemeSat(.55f-planetX*2.3f); \
            float sr=.002f+.014f*cloud+star*.65f, sg=.004f+.009f*cloud+star*.8f, sb=.012f+.04f*cloud+star; \
            sr=sr*(1.0f-planet)+planet*.04f*bands*light; sg=sg*(1.0f-planet)+planet*.16f*bands*light; sb=sb*(1.0f-planet)+planet*.32f*light; \
            R=R*(1.0f-aperture)*(1.0f+.5f*rim)+aperture*sr*.2f; G=G*(1.0f-aperture)*(1.0f+.5f*rim)+aperture*sg*.2f; B=B*(1.0f-aperture)*(1.0f+.5f*rim)+aperture*sb*.2f; \
            cyan=ThemeMax(cyan,inner*.45f); if(aperture>.5f) part=4; \
            float bolts=ThemePulse(x,.25f,.032f,fa)*ThemeBar(y,1.3f,.04f,fb); R*=1.0f+.2f*bolts; G*=1.0f+.3f*bolts; B*=1.0f+.4f*bolts; \
        } else { \
            float col=ThemeFloor((x+.72f)/.14f),height=.2f+.48f*ThemeHash((int)col,31); \
            float towers=ThemeSat((height-(y-1.45f))/fb+.5f); \
            float litWindows=ThemePulse(x,.14f,.052f,fa)*ThemePulse(y,.10f,.035f,fb)*towers; \
            float red=.012f+.14f*litWindows,green=.015f+.19f*litWindows,blue=.06f+.24f*litWindows; \
            R=R*(1.0f-fixture)+fixture*red*gain; G=G*(1.0f-fixture)+fixture*green*gain; B=B*(1.0f-fixture)+fixture*blue*gain; \
            pink=ThemeMax(pink,border*.8f); if(fixture>.5f) part=3; \
            float fins=ThemePulse(a,.12f,.035f,fa)*(1.0f-fixture)*ThemeBar(y,1.8f,2.6f,fb); R*=1.0f-.5f*fins; G*=1.0f-.5f*fins; B*=1.0f-.5f*fins; \
        } \
        float control=ThemeBar(x,.83f,.25f,fa)*ThemeBar(y,.93f,.38f,fb); \
        float keys=ThemePulse(x,.06f,.025f,fa)*ThemePulse(y,.07f,.026f,fb)*control; \
        cyan=ThemeMax(cyan,keys*.5f); amber=ThemeMax(amber,ThemeBar(x,.83f,.09f,fa)*ThemeBar(y,1.08f,.026f,fb)); \
    } \
    R+=gain*(cyan*.015f+pink*.75f+amber*.7f); G+=gain*(cyan*.55f+pink*.015f+amber*.25f); B+=gain*(cyan*.85f+pink*.42f+amber*.025f); \
    if(part==1 && ThemeMax(cyan,ThemeMax(pink,amber))>.2f) part=2; \
    return part; \
}

#define ThemeRef(name) float& name
VRX_ROOM_FUTURE_CODE
#undef ThemeRef
#define VRX_FUTURE_STRING_RAW(...) #__VA_ARGS__
#define VRX_FUTURE_STRING(...) VRX_FUTURE_STRING_RAW(__VA_ARGS__)
inline std::string RoomFutureHlsl() { return VRX_FUTURE_STRING(VRX_ROOM_FUTURE_CODE); }
#undef VRX_FUTURE_STRING
#undef VRX_FUTURE_STRING_RAW
#undef VRX_ROOM_FUTURE_CODE
