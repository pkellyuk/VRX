#pragma once
#include <string>

inline std::string RoomCompanionRigsHlsl() { return R"HLSL(
float2 CompanionPart(float2 current,float distance,float material)
{
    return distance<current.x?float2(distance,material):current;
}

float3 CompanionHead(float3 p,float t)
{
    float3 q=p-float3(0,.40,0);
    float sy,cy,sp,cp;
    sincos(.20*sin(t*1.4)+.08*sin(t*2.8),sy,cy);
    sincos(.11*sin(t*1.9+.8),sp,cp);
    q.xz=float2(cy*q.x-sy*q.z,sy*q.x+cy*q.z);
    q.yz=float2(cp*q.y+sp*q.z,cp*q.z-sp*q.y);
    return q;
}

// All endpoints are evaluated from their parent joint and a fixed bone length.
// The clothes, limbs and surface details follow the same pose during tracing
// and shading, including while Vrex turns or leans along its flight path.
float3 CompanionBone(float3 origin,float3 direction,float length)
{
    return origin+normalize(direction)*length;
}

float CompanionWingPanel(float3 p,float3 a,float3 b,float3 c)
{
    float3 n=normalize(cross(b-a,c-a));
    float face=abs(dot(n,p-a))-.011;
    float side=max(dot(normalize(cross(b-a,n)),p-a),dot(normalize(cross(c-b,n)),p-b));
    return max(face,max(side,dot(normalize(cross(a-c,n)),p-c)));
}

float2 CompanionDemonWing(float2 shape,float3 p,float side,float t,float energy)
{
    float flap=sin(t*(2.7+energy*.9)+.08*side);
    float3 root=float3(side*.15,.17,-.088);
    float3 elbow=CompanionBone(root,float3(side,.85+flap*.53,-.25),.44);
    float3 wrist=CompanionBone(elbow,float3(side,.13+flap*.84,-.32),.37);
    float3 tip=CompanionBone(wrist,float3(side,-.40+flap*.48,-.49),.31);
    float3 mid=CompanionBone(wrist,float3(side*.21,-.88+flap*.35,-.38),.37);
    float3 lower=CompanionBone(elbow,float3(side*.31,-.98+flap*.2,-.40),.41);
    float3 inner=float3(side*.20,-.24,-.19);
    float membrane=min(CompanionWingPanel(p,root,elbow,inner),
        min(CompanionWingPanel(p,elbow,wrist,lower),CompanionWingPanel(p,elbow,lower,inner)));
    membrane=min(membrane,min(CompanionWingPanel(p,wrist,tip,mid),CompanionWingPanel(p,wrist,mid,lower)));
    shape=CompanionPart(shape,membrane,8);
    float spars=min(LifeCapsule(p,root,elbow,.036),LifeCapsule(p,elbow,wrist,.025));
    spars=min(spars,min(LifeCapsule(p,wrist,tip,.014),LifeCapsule(p,wrist,mid,.017)));
    spars=min(spars,LifeCapsule(p,elbow,lower,.021));
    return CompanionPart(shape,spars,9);
}

float2 LifeCompanionRig(float3 p,float t,bool demon)
{
    uint gesture=((uint)vrexState>>8)&3;
    float energy=saturate(abs(sin(t*1.1))*.30+audioBass*.55);
    float3 h=CompanionHead(p,t);
    float3 cheeks=h-float3(0,.18,.066);
    float face=LifeBlend(LifeEllipsoid(cheeks,float3(.174,.192,.142)),
        LifeEllipsoid(h-float3(0,.106,.082),float3(.126,.106,.114)),.027);
    face=LifeBlend(face,LifeEllipsoid(h-float3(0,.146,.192),float3(.024,.038,.028)),.019);
    float2 shape=float2(face,1);
    shape=CompanionPart(shape,LifeCapsule(p,float3(0,.245,0),float3(0,.427,.017),.073),1);
    shape=CompanionPart(shape,LifeEllipsoid(p-float3(0,.070,0),float3(.194,.255,.121)),3);
    shape=CompanionPart(shape,LifeEllipsoid(p-float3(0,-.164,.001),float3(.161,.150,.117)),3);
    float3 waist=p-float3(0,-.153,0);
    shape=CompanionPart(shape,LifeRoundBox(waist,float3(.136,.044,.093),.026),4);
    shape=CompanionPart(shape,LifeEllipsoid(waist-float3(0,.004,.115),float3(.057,.038,.020)),5);
    float3 collar=p-float3(0,.236,.104);
    shape=CompanionPart(shape,LifeCapsule(collar,float3(-.106,.059,-.010),float3(.044,-.082,.018),.018),5);
    shape=CompanionPart(shape,LifeCapsule(collar,float3(.106,.059,-.010),float3(-.030,-.065,.019),.016),4);
    shape=CompanionPart(shape,LifeTorus(p-float3(0,.316,.006),.072,.013),4);
    shape=CompanionPart(shape,LifeEllipsoid(p-float3(0,.31,.083),float3(.021,.029,.014)),5);

    if(!demon)
    {
        // Wrapped collar, raised sash ornament and layered pleats. Hair is
        // sculpted as a fitted cap, tapered fringe, and hinged back locks.
        float y=p.y+.21, drop=saturate(-y/.315);
        float pleats=.013*cos(atan2(p.x,p.z)*16);
        float skirt=max(length(p.xz*float2(1,1.40))-(.15+.16*drop+pleats),max(y,-y-.315));
        shape=CompanionPart(shape,skirt,3);
        float hem=max(length(p.xz*float2(1,1.46))-(.286+pleats*.7),max(y+.294,-y-.339));
        shape=CompanionPart(shape,hem,4);
        float3 hair=h-float3(0,.239,-.025);
        float cap=max(LifeEllipsoid(hair,float3(.200,.218,.191)),.028-h.y-.61*max(0,-h.z)+.92*max(0,h.z));
        shape=CompanionPart(shape,cap,2);
        float fringeX=abs(h.x+.019);
        float fringeY=.210+fringeX*.26+.017*sin(h.x*39)+.009*sin(h.x*72);
        float fringe=max(LifeEllipsoid(h-float3(-.026,.278,.107),float3(.166,.157,.092)),fringeY-h.y);
        shape=CompanionPart(shape,fringe,2);
    }
    else
    {
        shape=CompanionPart(shape,LifeEllipsoid(h-float3(0,.305,-.026),float3(.180,.132,.145)),2);
        float3 tailRoot=float3(0,-.225,-.100);
        float3 tailBend=CompanionBone(tailRoot,float3(.42*sin(t*1.8),-.74,-.92),.35);
        float3 tailTip=CompanionBone(tailBend,float3(.70*cos(t*1.8),-.27,-1),.36);
        float tail=min(LifeCapsule(p,tailRoot,tailBend,.036),LifeCapsule(p,tailBend,tailTip,.022));
        shape=CompanionPart(shape,tail,9);
        shape=CompanionPart(shape,LifeEllipsoid(p-tailTip,float3(.045,.038,.070)),4);
    }

    [loop] for(int limb=0;limb<2;++limb)
    {
        float side=limb==0?-1:1;
        float cycle=sin(t*2.5+side*1.55);
        float3 shoulder=float3(side*.153,.207,0);
        float3 upper=float3(side*.44,-1,.08+.27*cycle);
        float3 lower=float3(side*.22,-.90,.42+.30*cycle);
        if(gesture==1 && limb==0) {upper=float3(-.91,-.23,.29);lower=float3(-.18+.23*sin(t*9),1,.29);}
        if(gesture==2) {upper=float3(side*.62,-.90,.13);lower=float3(side*.52,.31,.54);}
        if(gesture==3 && limb==1) {upper=float3(.49,-.40,.78);lower=float3(.16,.09,1);}
        float3 elbow=CompanionBone(shoulder,upper,.236);
        float3 wrist=CompanionBone(elbow,lower,.211);
        float3 cuff=lerp(elbow,wrist,.79);
        shape=CompanionPart(shape,LifeCapsule(p,shoulder,elbow,.070),3);
        shape=CompanionPart(shape,LifeCapsule(p,elbow,cuff,.052),3);
        shape=CompanionPart(shape,LifeCapsule(p,cuff,lerp(elbow,wrist,.88),.055),5);
        float3 hand=CompanionBone(wrist,lower,.044);
        float3 palm=p-hand;
        float thumb=LifeCapsule(palm,float3(-side*.020,-.005,.006),float3(-side*.043,-.021,.025),.015);
        shape=CompanionPart(shape,min(thumb,LifeEllipsoid(palm,float3(.035,.052,.032))),1);
        if(gesture==3 && limb==1)
            shape=CompanionPart(shape,LifeCapsule(p,hand,CompanionBone(hand,lower,.075),.015),1);

        float3 hip=float3(side*.089,-.237,0);
        float3 knee=CompanionBone(hip,float3(side*.18,-1,.18+.40*cycle),.304);
        float3 ankle=CompanionBone(knee,float3(side*.067,-1,-.20+cycle*.31),.286);
        float3 bootTop=lerp(knee,ankle,.24);
        float leg=min(LifeCapsule(p,hip,knee,.070),LifeCapsule(p,knee,bootTop,.061));
        if(!demon) leg=max(leg,p.y+.472);
        shape=CompanionPart(shape,leg,6);
        shape=CompanionPart(shape,LifeCapsule(p,bootTop,ankle,.057),7);
        shape=CompanionPart(shape,LifeCapsule(p,bootTop,lerp(knee,ankle,.30),.063),4);
        float3 toe=ankle+float3(0,-.012,.061);
        shape=CompanionPart(shape,LifeRoundBox(p-toe,float3(.049,.028,.075),.023),7);
        shape=CompanionPart(shape,LifeRoundBox(p-toe-float3(0,.031,.029),float3(.044,.011,.012),.009),4);

        float3 ear=float3(side*.173,.164,-.005);
        shape=CompanionPart(shape,LifeEllipsoid(h-ear,float3(.029,.047,.034)),1);
        if(demon)
        {
            float3 horn=float3(side*.125,.328,-.037);
            float3 ridge=float3(side*.208,.465,-.097);
            shape=CompanionPart(shape,LifeCapsule(h,horn,ridge,.043),9);
            shape=CompanionPart(shape,LifeCapsule(h,ridge,float3(side*.217,.585,-.160),.023),9);
            shape=CompanionDemonWing(shape,p,side,t,energy);
        }
        else
        {
            float sway=sin(t*2.5+side)*.045;
            float3 lockA=float3(side*.161,.226,-.040);
            float3 lockB=float3(side*.188+sway*.30,-.068,-.071);
            float3 lockC=float3(side*.241+sway,-.196,-.128);
            shape=CompanionPart(shape,LifeCapsule(h,lockA,lockB,.046),2);
            shape=CompanionPart(shape,LifeCapsule(h,lockB,lockC,.029),2);
            shape=CompanionPart(shape,LifeEllipsoid(h-(ear+float3(0,-.055,.014)),float3(.014,.027,.015)),5);
        }
    }
    if(!demon)
    {
        float3 pin=h-float3(.123,.283,.140);
        shape=CompanionPart(shape,LifeCapsule(pin,float3(-.034,-.013,0),float3(.027,.021,-.001),.010),5);
        float3 flower=pin-float3(.018,.014,.007);
        float petals=.024+.006*cos(atan2(flower.y,flower.x)*5);
        shape=CompanionPart(shape,max(length(flower.xy)-petals,abs(flower.z)-.009),4);
        shape=CompanionPart(shape,length(flower-float3(0,0,.011))-.010,5);
    }
    return shape;
}

float3 LifeCompanionColour(float3 p,float t,bool demon,float shade,float highlight,float rim)
{
    int material=(int)LifeCompanionRig(p,t,demon).y;
    float3 colour=float3(.037,.019,.054);
    if(material==1) colour=demon?float3(.40,.09,.10):float3(.92,.55,.52);
    if(material==2)
    {
        float hair=.76+.24*pow(.5+.5*cos(p.x*105+p.y*10+p.z*31),3);
        colour=lerp(float3(.018,.013,.034),float3(.08,.029,.122),hair*.6);
        highlight*=hair;
    }
    if(material==3)
    {
        float weave=.90+.10*cos(p.x*170)*cos(p.y*160);
        colour=(demon?float3(.092,.028,.034):float3(.088,.025,.120))*weave;
        if(!demon && p.y<-.25)
        {
            float band=(p.y+.30)*21;
            float turn=atan2(p.z,p.x)*3.2;
            float2 stitch=frac(float2(turn+.5*floor(band),band))-.5;
            float flower=.135+.063*cos(5*atan2(stitch.y,stitch.x));
            float thread=smoothstep(.025,.006,abs(length(stitch)-flower));
            colour+=thread*float3(.15,.037,.085);
        }
    }
    if(material==4) colour=demon?float3(.64,.054,.045):float3(.46,.059,.19);
    if(material==5) colour=float3(.80,.43,.13);
    if(material==6) colour=float3(.10,.068,.13);
    if(material==7) colour=float3(.035,.022,.050);
    if(material==8) colour=float3(.31,.024,.052)*(1+.22*sin(p.y*22+p.x*9));
    if(material==9) colour=float3(.19,.045,.059)*(.86+.14*cos(p.y*115));
    float3 light=colour*(shade+.24)+highlight*(material==5?.40:.10)+colour*rim*.24;

    float3 h=CompanionHead(p,t);
    if(material==1 && h.z>.137 && p.y>.43)
    {
        float eyelid=frac(t*.233)>.956?.10:1;
        if((((uint)vrexState>>8)&3)==1 && h.x<0 && frac(t*.87)<.31) eyelid=.11;
        float2 eye=float2((abs(h.x)-.082)/.054,(h.y-.173-.11*(abs(h.x)-.082))/.031);
        float almond=abs(eye.y)/eyelid+pow(abs(eye.x),1.55);
        if(almond<1.14) light=float3(.025,.009,.025);
        if(almond<.87)
        {
            light=lerp(float3(.90,.71,.71),float3(.07,.004,.021),saturate((.70-abs(eye.x))*11));
            if(length(float2(eye.x+.06,eye.y))<.38) light=demon?float3(1.6,.22,.028):float3(.55,.035,.14);
            if(abs(eye.x+.06)<.10 && abs(eye.y)<.36) light=float3(.020,.005,.019);
            if(length(eye-float2(-.16,.15))<.15) light+=.65;
        }
        if(abs(eye.x)<1.05 && abs(h.y-(.220+.008*(1-eye.x*eye.x)))<.008) light*=.33;
        float smile=.059+.027*pow(saturate(abs(h.x)/.066),2);
        float talk=.008+((vrexChat>0)?(.005+.010*abs(sin(t*11))):0);
        if(abs(h.x)<.066 && abs(h.y-smile)<talk) light=float3(.20,.019,.047);
        if(abs(abs(h.x)-.047)<.010 && h.y<smile+.003 && h.y>smile-.015) light=float3(.93,.82,.74);
    }
    return light;
}
)HLSL"; }
