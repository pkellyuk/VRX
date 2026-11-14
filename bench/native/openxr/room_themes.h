#pragma once

// One scalar material program for the CPU reference and HLSL. Fine detail uses
// the existing analytic pixel-footprint filters, including at grazing angles.
#define VRX_ROOM_THEME_CODE \
float ThemeSat(float v) { return ThemeMin(ThemeMax(v,0.0f),1.0f); } \
float ThemeArch(float x, float y, float radius, float spring, float foot) { \
    float top = spring + ThemeSqrt(ThemeMax(0.0f,radius*radius-x*x)); \
    return ThemeSat(0.5f-ThemeMax(ThemeAbs(x)-radius,y-top)/foot)*ThemeSat((y-.12f)/foot); \
} \
int ThemeSurface(int env, int face, float a, float b, float floorY, float fa, float fb, float lamp, ThemeRef(R), ThemeRef(G), ThemeRef(B)) { \
    bool hell = env == 6; \
    bool horizontal = face == 3 || face == 4; \
    float y = horizontal ? b : b-floorY; \
    float foot = ThemeMax(.001f,ThemeMax(fa,fb)); \
    float pitchX = horizontal ? .9f : .78f, pitchY = horizontal ? .72f : .38f; \
    float row = ThemeFloor(y/pitchY), aa = a + (row-2.0f*ThemeFloor(row*.5f))*.5f*pitchX; \
    float cell = ThemeFloor(aa/pitchX); \
    float fade = ThemeSat(1.0f-2.0f*foot/ThemeMin(pitchX,pitchY)); \
    float seamX = ThemePulse(aa,pitchX,.018f,fa), seamY = ThemePulse(y,pitchY,.016f,fb); \
    float seam = ThemeMax(seamX,seamY); \
    float bevel = ThemeMax(ThemePulse(aa,pitchX,.065f,fa),ThemePulse(y,pitchY,.058f,fb)); \
    float tone = .76f+.44f*ThemeHash((int)cell,(int)row); \
    float grain = ThemeFolds(a+y*.23f,.047f,fa+fb*.23f)*.055f + ThemeFolds(y-a*.31f,.083f,fb+fa*.31f)*.07f; \
    float stone = (1.0f-.78f*seam+.20f*(bevel-seam))*(1.0f+(tone-1.0f)*fade+grain); \
    R *= stone; G *= stone; B *= stone; \
    int part = 1; \
    if (!hell) { \
        if (horizontal) { \
            if (face == 3) { \
                float grate = ThemeBar(a,0.0f,.78f,fa)*ThemePulse(b,3.2f,1.0f,fb); \
                float iron = ThemeMax(ThemePulse(a,.13f,.025f,fa),ThemePulse(b,.16f,.025f,fb)); \
                float g = 1.0f-grate+grate*(.06f+.28f*iron); R*=g; G*=g; B*=g; \
                if (grate>.5f) part=3; \
            } else { \
                float rib = ThemeMax(ThemePulse(a,2.6f,.18f,fa),ThemePulse(b,2.6f,.22f,fb)); \
                float vault = .48f+.6f*rib+.13f*ThemeFolds(a,2.6f,fa); R*=vault; G*=vault; B*=vault; \
            } \
        } else { \
            float x = a-2.6f*ThemeFloor(a/2.6f+.5f); \
            float opening = ThemeArch(x,y,.66f,1.8f,foot); \
            float rim = ThemeMax(0.0f,ThemeArch(x,y,.84f,1.8f,foot)-opening); \
            float angle = ThemeAtan2(y-1.8f,x); \
            float joints = y>1.8f ? ThemePulse(angle,.24f,.018f,foot/.7f) : ThemePulse(y,.34f,.018f,fb); \
            float shade = (1.0f-opening-rim)+opening*.12f+rim*(1.35f-.65f*joints); \
            R*=shade; G*=shade; B*=shade; \
            float bars=opening*ThemeMax(ThemePulse(x,.18f,.027f,fa),ThemeBar(y,1.0f,.045f,fb)); \
            R*=1.0f-.6f*bars; G*=1.0f-.5f*bars; B*=1.0f-.4f*bars; \
            if (opening>.5f || rim>.5f) part=2; \
            float pier=ThemeBar(ThemeAbs(x),1.24f,.19f,fa); \
            float cap=ThemeMax(ThemeBar(y,.24f,.17f,fb),ThemeBar(y,2.76f,.22f,fb)); \
            float relief=1.0f+pier*(.35f+.35f*cap); R*=relief; G*=relief; B*=relief; \
            float damp=ThemeSat(1.0f-y/.85f)*(.5f+.5f*ThemeFolds(a,.61f,fa)); \
            R*=1.0f-.4f*damp; G*=1.0f-.15f*damp; B*=1.0f-.4f*damp; \
            float linkY=y-.14f*ThemeFloor(y/.14f)-.07f; \
            float chainDistance=ThemeSqrt((x+.97f)*(x+.97f)/(.028f*.028f)+linkY*linkY/(.055f*.055f)); \
            float chain=ThemeSat((.19f-ThemeAbs(chainDistance-1.0f))*.028f/foot+.5f)*ThemeSat((y-.7f)/foot); \
            R*=1.0f-.35f*chain; G*=1.0f-.25f*chain; B*=1.0f-.15f*chain; \
            float dx=x-1.0f, dy=y-1.86f; \
            float bracket=ThemeBar(dx,0.0f,.085f,fa)*ThemeBar(y,1.57f,.4f,fb); \
            R*=1.0f-.8f*bracket; G*=1.0f-.8f*bracket; B*=1.0f-.8f*bracket; \
            float flameX=dx+.016f*ThemeSin(dy*21.0f); \
            float flameWidth=.075f*ThemeSat((.23f-dy)/.38f); \
            float flame=ThemeSat((flameWidth-ThemeAbs(flameX))/foot+.5f)*ThemeBar(dy,.04f,.38f,fb); \
            float core=flame*ThemeSat((.028f-ThemeAbs(dx))/foot+.5f)*ThemeBar(dy,-.035f,.17f,fb); \
            float basket=ThemeBar(dx,0.0f,.16f,fa)*ThemeBar(y,1.69f,.085f,fb); \
            R*=1.0f-.8f*basket; G*=1.0f-.8f*basket; B*=1.0f-.8f*basket; \
            float halo=ThemeExp(-dx*dx*3.2f-dy*dy*2.2f)*.23f*lamp; \
            R+=halo+lamp*.95f*flame; G+=halo*.48f+lamp*(.31f*flame+.42f*core); B+=halo*.14f+lamp*(.025f*flame+.07f*core); \
            if (flame>.25f || bracket>.5f) part=4; \
        } \
    } else { \
        float warp=.12f*ThemeSin(y*2.7f)+.045f*ThemeSin(y*9.0f); \
        float crackX=ThemePulse(aa+warp,pitchX,.035f,fa+fb*.45f); \
        float crackY=ThemePulse(y+.04f*ThemeSin(a*7.0f),pitchY,.025f,fb+fa*.28f); \
        float fissure=ThemeMax(crackX,crackY)*(.35f+.65f*ThemeHash((int)cell,(int)row)); \
        float lava=fissure*.24f, glow=fissure*.03f; \
        if (face==3) { \
            float channel=ThemeAbs(a)-1.8f-.24f*ThemeSin(b*1.3f)-.1f*ThemeSin(b*3.6f); \
            float river=ThemeBar(channel,0.0f,.32f,fa+fb*.7f); \
            float crust=.70f+.30f*ThemeFolds(b+a*.4f,.23f,fb+fa*.4f); \
            lava=ThemeMax(lava,river*crust); glow+=.12f*ThemeExp(-channel*channel*2.8f); \
            R*=1.0f-river*.82f; G*=1.0f-river*.82f; B*=1.0f-river*.82f; \
            float radius=ThemeSqrt(a*a+(b-2.8f)*(b-2.8f)); \
            float seal=ThemeBar(radius,.82f,.027f,foot)+ThemeBar(radius,.70f,.014f,foot); \
            float angle=radius>.0001f ? ThemeAtan2(b-2.8f,a) : 0.0f; \
            seal=ThemeMax(seal,ThemePulse(angle,1.256637f,.025f,foot/ThemeMax(radius,.05f))*ThemeBar(radius,.6f,.37f,foot)); \
            lava=ThemeMax(lava,ThemeSat(seal)*.38f); \
        } else if (face!=4) { \
            float x=a-3.0f*ThemeFloor(a/3.0f+.5f); \
            float opening=ThemeArch(x,y,.8f,2.15f,foot); \
            float rim=ThemeMax(0.0f,ThemeArch(x,y,1.01f,2.15f,foot)-opening); \
            float shade=1.0f-opening*.94f+rim*.5f; R*=shade; G*=shade; B*=shade; lava*=1.0f-opening; \
            float ridge=.48f+.20f*ThemeSin(x*8.0f)+.14f*ThemeSin(x*17.0f); \
            float silhouettes=ThemeSat((ridge-y)/foot+.5f); \
            float furnace=opening*(.025f+.12f*ThemeExp(-ThemeAbs(x)*3.0f-y*.8f))*(1.0f-.9f*silhouettes); \
            float grate=opening*ThemePulse(x,.23f,.038f,fa); furnace*=1.0f-grate*.95f; \
            glow+=furnace; \
            float rune=ThemePulse(x,.30f,.022f,fa)*ThemeBar(y,3.18f,.17f,fb); \
            rune=ThemeMax(rune,ThemePulse(x,.6f,.13f,fa)*ThemeBar(y,3.14f,.024f,fb)); lava=ThemeMax(lava,rune*.65f); \
            float pier=ThemeBar(ThemeAbs(x),1.38f,.19f,fa); R*=1.0f+.5f*pier; G*=1.0f+.4f*pier; B*=1.0f+.4f*pier; \
            if (opening>.5f || rim>.5f) part=2; \
            if (rune>.5f) part=6; \
        } else { lava*=.25f; glow+=.016f; } \
        float hot=ThemeSat(lava); R+=lamp*(hot*1.7f+glow); G+=lamp*(hot*.24f+glow*.12f); B+=lamp*(hot*.012f+glow*.025f); \
        if (lava>.14f && part!=6) part=5; \
    } \
    return part; \
}

inline float ThemeMin(float a,float b) { return std::fmin(a,b); }
inline float ThemeMax(float a,float b) { return std::fmax(a,b); }
inline float ThemeAbs(float a) { return std::fabs(a); }
inline float ThemeSqrt(float a) { return std::sqrt(a); }
inline float ThemeFloor(float a) { return std::floor(a); }
inline float ThemeSin(float a) { return std::sin(a); }
inline float ThemeExp(float a) { return std::exp(a); }
inline float ThemeAtan2(float a,float b) { return std::atan2(a,b); }
inline float ThemePulse(float a,float b,float c,float d) { return RoomPulse(a,b,c,d); }
inline float ThemeBar(float a,float b,float c,float d) { return RoomBar(a,b,c,d); }
inline float ThemeFolds(float a,float b,float c) { return RoomFolds(a,b,c); }
inline float ThemeHash(int a,int b) { return float(RoomHash(a,b))/65535.0f; }
#define ThemeRef(name) float& name
VRX_ROOM_THEME_CODE
#undef ThemeRef

#define VRX_THEME_STRING_RAW(...) #__VA_ARGS__
#define VRX_THEME_STRING(...) VRX_THEME_STRING_RAW(__VA_ARGS__)
inline std::string RoomThemeHlsl()
{
    return std::string(
        "#define ThemeMin min\n#define ThemeMax max\n#define ThemeAbs abs\n#define ThemeSqrt sqrt\n"
        "#define ThemeFloor floor\n#define ThemeSin sin\n#define ThemeExp exp\n#define ThemeAtan2 atan2\n"
        "#define ThemePulse PulseH\n#define ThemeBar BarH\n#define ThemeFolds FoldsH\n#define ThemeRef(name) inout float name\n"
        "float ThemeHash(int a,int b) { return (float)HashH(a,b)/65535.0; }\n") + VRX_THEME_STRING(VRX_ROOM_THEME_CODE) + "\n";
}
#undef VRX_THEME_STRING
#undef VRX_THEME_STRING_RAW
#undef VRX_ROOM_THEME_CODE
