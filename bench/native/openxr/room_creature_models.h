#pragma once
#include <string>
#include "room_companion_rigs.h"

// All models fit their collision sphere, including moving appendages. Animation
// clocks come from the simulation, so gait rate follows speed without abrupt turns.
inline std::string RoomCreatureModelsHlsl() { return R"HLSL(
float LifeRoundBox(float3 p,float3 b,float r) { return LifeBox(p,b)-r; }
float LifeCapsule(float3 p,float3 a,float3 b,float r)
{ float3 ab=b-a;return length(p-a-ab*saturate(dot(p-a,ab)/max(dot(ab,ab),.00001)))-r; }
float LifeTorus(float3 p,float major,float minor) { return length(float2(length(p.xz)-major,p.y))-minor; }
float LifeBlend(float a,float b,float k) { float h=saturate(.5+.5*(b-a)/k);return lerp(b,a,h)-k*h*(1-h); }
float LifeFin(float3 p,float3 size) { return max(abs(p.y)-size.y,max(abs(p.x)-size.x,max(-p.z-size.z,p.z-size.z*(1-abs(p.x)/size.x)))); }
float LifeRobotModel(float3 p,float t,bool hero)
{
    float gait=sin(t*2.3), sit=hero && vrexState==2?1:0;
    float wave=hero && vrexChat==4?1:0;
    float a=LifeRoundBox(p-float3(0,-.04,0),float3(.25,.24,.18),.055);
    a=min(a,LifeRoundBox(p-float3(0,-.02,.218),float3(.245,.20,.025),.018));
    a=min(a,LifeCapsule(p,float3(0,.22,0),float3(0,.31,0),.09));
    a=min(a,LifeRoundBox(p-float3(0,.48,.015),float3(.31,.195,.235),.06));
    a=min(a,LifeRoundBox(p-float3(0,.49,.244),float3(.275,.145,.028),.05));
    a=min(a,LifeCapsule(p,float3(.20,.70,-.10),float3(.24,.86,-.12),.021));
    a=min(a,length(p-float3(.24,.87,-.12))-.04);
    [loop] for(uint limb=0;limb<2;++limb)
    {
        float side=limb==0?-1:1, step=gait*side*(1-sit);
        float3 hip=float3(side*.13,-.31,0),knee=float3(side*.14,-.55,.09+step*.10+sit*.20);
        float3 ankle=float3(side*.14,-.79,step*.14+sit*.27);
        a=min(a,LifeCapsule(p,hip,knee,.078));a=min(a,length(p-knee)-.097);
        a=min(a,LifeCapsule(p,knee,ankle,.063));
        a=min(a,LifeRoundBox(p-ankle-float3(0,-.065,.04),float3(.09,.055,.13),.025));
        float3 shoulder=float3(side*.345,.13,0),elbow=float3(side*.42,-.09,-step*.12),hand=float3(side*.43,-.34,-step*.15);
        if(wave>0 && side<0) {elbow=float3(-.45,.31,0);hand=float3(-.50,.52+sin(t*8)*.055,.02);}
        a=min(a,length(p-shoulder)-.092);a=min(a,LifeCapsule(p,shoulder,elbow,.065));
        a=min(a,length(p-elbow)-.075);a=min(a,LifeCapsule(p,elbow,hand,.057));
        a=min(a,LifeRoundBox(p-hand,float3(.068,.065,.045),.019));
        if(hero) [loop] for(int finger=0;finger<3;++finger)
            a=min(a,LifeCapsule(p,hand+float3((finger-1)*.04,-.045,.025),hand+float3((finger-1)*.04,-.12,.055),.012));
        float3 ear=p-float3(side*.365,.48,.015);
        a=min(a,LifeEllipsoid(ear,float3(.04,.105,.105)));
        if(hero)
        {
            // Twin jet cans, protective collars and visible nozzle recesses.
            float3 jet=p-float3(side*.19,-.02,-.29);
            float can=LifeCapsule(jet,float3(0,-.18,0),float3(0,.17,0),.105);
            can=max(can,-LifeCapsule(jet,float3(0,-.30,0),float3(0,-.17,0),.066));
            a=min(a,can);a=min(a,LifeTorus(jet-float3(0,-.13,0),.105,.018));
            a=min(a,LifeTorus(jet-float3(0,.14,0),.105,.018));
            if(sit<.5) a=min(a,LifeEllipsoid(jet-float3(0,-.34,0),float3(.053,.12+.025*sin(t*28+side),.053)));
        }
    }
    if(hero) a=min(a,LifeRoundBox(p-float3(0,.04,-.26),float3(.12,.19,.07),.035));
    return a;
}
// Reconstructed from the user's PersonaBot teal mascot artwork: oversized
// rounded head, inset navy face, headphone ears, centre antenna and soft limbs.
float3 LifeMascotHeadSpace(float3 p,float t)
{
    float3 pivot=float3(0,.06,0);
    float3 q=p-pivot;
    float yaw=.19*sin(t*1.45)+.06*sin(t*3.2);
    float pitch=.12*sin(t*1.9+.6);
    float sy,cy,sp,cp;sincos(yaw,sy,cy);sincos(pitch,sp,cp);
    q.xz=float2(cy*q.x-sy*q.z,sy*q.x+cy*q.z);
    q.yz=float2(cp*q.y+sp*q.z,cp*q.z-sp*q.y);
    return q+pivot;
}
float LifeMascotModel(float3 p,float t)
{
    float3 head=LifeMascotHeadSpace(p,t);
    float a=LifeRoundBox(head-float3(0,.34,0),float3(.29,.21,.13),.14);
    a=min(a,LifeRoundBox(head-float3(0,.34,.245),float3(.255,.175,.014),.085));
    a=min(a,LifeRoundBox(p-float3(0,-.29,0),float3(.19,.18,.12),.09));
    a=min(a,LifeCapsule(p,float3(0,-.03,0),float3(0,.03,0),.085));
    a=min(a,LifeCapsule(head,float3(0,.66,-.03),float3(0,.84,-.03),.019));
    a=min(a,length(head-float3(0,.855,-.03))-.052);
    [loop] for(uint side=0;side<2;++side)
    {
        float sign=side==0?-1:1;
        a=min(a,LifeRoundBox(head-float3(sign*.46,.34,0),float3(.035,.10,.075),.045));
        uint gesture=((uint)vrexState>>8)&3;
        float swing=sin(t*2.8+sign*1.5);
        float3 elbow=float3(sign*(.38+.025*swing),-.30+.035*swing,.07*swing);
        float3 hand=float3(sign*(.43+.04*swing),-.44+.09*swing,.06+.13*swing);
        if((gesture==1 || vrexChat==4) && side==0) {elbow=float3(-.48,.06,.02);hand=float3(-.50+.045*sin(t*8),.28+.06*cos(t*8),.03);}
        if(gesture==2) {elbow=float3(sign*.42,-.08,.03);hand=float3(sign*.54,.02+.03*sin(t*3),.09);}
        if(gesture==3 && side==1)
        {
            elbow=float3(.34,-.07,.18);hand=float3(.31,.02,.48);
            a=min(a,LifeCapsule(p,hand+float3(0,.025,.03),float3(.29,.045,.65),.024));
        }
        a=min(a,LifeCapsule(p,float3(sign*.27,-.12,0),elbow,.061));
        a=min(a,LifeCapsule(p,elbow,hand,.055));a=min(a,LifeEllipsoid(p-hand,float3(.079,.075,.060)));
        float stride=sin(t*2.8-sign*1.5);
        float3 knee=float3(sign*.14,-.65+max(stride,0)*.045,.10*stride);
        float3 ankle=float3(sign*.16,-.805+max(stride,0)*.09,.16*stride-.025);
        a=min(a,LifeCapsule(p,float3(sign*.12,-.51,0),knee,.071));
        a=min(a,LifeCapsule(p,knee,ankle,.061));
        a=min(a,LifeRoundBox(p-ankle-float3(0,-.04,.055),float3(.08,.025,.10),.048));
        float3 jet=p-float3(sign*.17,-.27,-.24);
        a=min(a,LifeCapsule(jet,float3(0,-.13,0),float3(0,.13,0),.088));
        a=min(a,LifeTorus(jet-float3(0,-.11,0),.085,.013));
        a=min(a,LifeEllipsoid(jet-float3(0,-.30,0),float3(.045,.10+.015*sin(t*30),.045)));
    }
    return a;
}
)HLSL" + RoomCompanionRigsHlsl() + R"HLSL(
float LifeShape(float3 p,int k,float phase,float anim)
{
    float t=anim+phase, swing=sin(t*2.1), beat=audioBass*.025;
    if(k>=9) return LifeCompanionRig(p,t,k==10).x;
    float a=1;
    if(k==0 || k==1)
    {
        p.x-=sin(p.z*4.5-t*2.1)*.055*(1-saturate(p.z+.3));
        a=LifeEllipsoid(p,k==0?float3(.225,.20,.69):float3(.27,.33,.53));
        if(k==0) a=LifeBlend(a,LifeEllipsoid(p-float3(0,-.025,.49),float3(.17,.12,.31)),.06);
        float3 fin=p-float3(0,-.075,-.045);fin.y+=abs(fin.x)*.12;
        a=min(a,LifeFin(fin,float3(.56,.025,.25)));
        float3 dorsal=float3(p.z+.08,p.x,p.y-.27);
        a=min(a,LifeFin(dorsal,float3(.20,.025,.26)));
        float3 tail=p-float3(swing*.08,0,-.65);
        a=min(a,LifeCapsule(tail,float3(0,0,.08),float3(0,0,-.10),.048));
        a=min(a,LifeEllipsoid(tail-float3(0,.16,-.07),float3(.033,.22,.09)));
        a=min(a,LifeEllipsoid(tail-float3(0,-.14,-.07),float3(.033,.18,.09)));
        // A real mouth crease catches the light below the snout.
        if(k==0) a=max(a,-LifeRoundBox(p-float3(0,-.08,.635),float3(.14,.009,.08),.008));
    }
    else if(k==2) a=LifeRobotModel(p,t,false);
    else if(k==8) a=LifeMascotModel(p,t);
    else if(k==3)
    {
        float pulse=1+.07*sin(t*2.2);
        float outer=LifeEllipsoid(p-float3(0,.26,0),float3(.53*pulse,.34,.53*pulse));
        float inner=LifeEllipsoid(p-float3(0,.22,0),float3(.47*pulse,.30,.47*pulse));
        a=max(max(outer,-inner),.04-p.y);
        a=min(a,LifeTorus(p-float3(0,.08,0),.48*pulse,.032));
        a=min(a,LifeEllipsoid(p-float3(0,.24,0),float3(.12,.16,.12)));
        [loop] for(uint j=0;j<8;++j)
        {
            float angle=j*.785398;
            float radius=.28+.07*sin(p.y*7-t*2.2+angle);
            float2 offset=float2(cos(angle),sin(angle))*radius;
            float strand=length(p.xz-offset)-(.016+.015*saturate(p.y+.8));
            a=min(a,max(strand,max(-p.y-.77,p.y-.17)));
        }
    }
    else if(k==4)
    {
        p.y-=sin(abs(p.x)*3-t*1.7)*abs(p.x)*.17;
        a=LifeEllipsoid(p,float3(.31,.14,.48));
        float3 wing=p;wing.z+=abs(p.x)*.22;
        a=LifeBlend(a,LifeEllipsoid(wing,float3(.81,.047,.39)),.08);
        float3 tail=p;tail.x-=sin(t*1.7+p.z*5)*.035;tail.z+=.57;
        a=min(a,LifeCapsule(tail,float3(0,0,.12),float3(0,0,-.31),.021));
        a=min(a,LifeCapsule(p,float3(-.13,0,.41),float3(-.20,.02,.59),.032));
        a=min(a,LifeCapsule(p,float3(.13,0,.41),float3(.20,.02,.59),.032));
    }
    else if(k==5)
    {
        a=LifeEllipsoid(p-float3(0,-.02,-.18),float3(.20,.20,.32));
        a=min(a,LifeEllipsoid(p-float3(0,.04,.18),float3(.15,.18,.17)));
        a=min(a,length(p-float3(0,.06,.38))-.14);
        [loop] for(uint side=0;side<2;++side)
        {
            float sign=side==0?-1:1;
            float3 wing=p-float3(sign*.39,.11+sin(t*13)*.13,-.02);
            wing.y+=sign*wing.x*sin(t*13)*.35;
            a=min(a,LifeEllipsoid(wing,float3(.32,.018,.25)));
            a=min(a,LifeCapsule(p,float3(sign*.07,.14,.43),float3(sign*.20,.31,.57),.016));
            [loop] for(uint leg=0;leg<3;++leg)
            {
                float z=-.15+leg*.16;float bob=sin(t*3+leg)*.02;
                a=min(a,LifeCapsule(p,float3(sign*.10,-.08,z),float3(sign*.31,-.24+bob,z-.08),.018));
            }
        }
    }
    else if(k==6)
    {
        p.y+=.38;
        a=LifeEllipsoid(p-float3(0,-.03,0),float3(.51,.33,.57));
        a=min(a,LifeTorus((p-float3(0,-.16,0))*float3(1,1,.9),.48,.035));
        float3 head=p-float3(0,-.09,.62);head.y-=.025*sin(t*1.8);
        a=min(a,LifeEllipsoid(head,float3(.17,.14,.21)));
        [loop] for(uint j=0;j<4;++j)
        {
            float side=j%2==0?-1:1,z=j<2?.24:-.24,step=sin(t*2+j*3.14159)*.06;
            float3 foot=float3(side*.26,-.47,z+step);
            a=min(a,LifeCapsule(p,float3(side*.34,-.20,z),foot,.072));
            a=min(a,LifeEllipsoid(p-foot,float3(.11,.07,.12)));
        }
        a=min(a,LifeCapsule(p,float3(0,-.16,-.50),float3(0,-.20,-.68),.035));
    }
    else
    {
        a=LifeRoundBox(p,float3(.23,.12,.23),.06);
        a=min(a,LifeCapsule(p,float3(0,.13,0),float3(0,.36,-.03),.018));
        a=min(a,length(p-float3(0,.38,-.03))-.037);
        a=min(a,LifeEllipsoid(p-float3(0,-.18,.15),float3(.11,.10,.12)));
        [loop] for(uint j=0;j<4;++j)
        {
            float angle=j*1.570796+.785398;float3 hub=float3(cos(angle)*.48,.07,sin(angle)*.48);
            a=min(a,LifeCapsule(p,float3(0,0,0),hub,.034));
            float3 q=p-hub;a=min(a,LifeTorus(q,.19,.025));
            float2 blade=float2(q.x*cos(t*22)-q.z*sin(t*22),q.x*sin(t*22)+q.z*cos(t*22));
            a=min(a,LifeRoundBox(float3(blade.x,q.y,blade.y),float3(.16,.009,.018),.005));
            a=min(a,LifeCapsule(q,float3(0,-.02,0),float3(0,.04,0),.044));
        }
    }
    return max(a,length(p)-.985);
}
)HLSL"; }
