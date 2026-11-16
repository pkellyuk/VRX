#pragma once
#include <string>
#include "room_creature_models.h"
#include "vrex_branding.h"
inline std::string RoomLifeHlsl() { return R"HLSL(
float3 LifePalette(float h) { return .5+.5*cos(6.2831853*(h+float3(0,.333,.667))); }
float LifeHash(float n) { return frac(sin(n*127.1+311.7)*43758.5453); }
float LifeBox(float3 p,float3 b) { float3 q=abs(p)-b; return length(max(q,0))+min(max(q.x,max(q.y,q.z)),0); }
float LifeEllipsoid(float3 p,float3 r) { float a=length(p/r), b=length(p/(r*r)); return a*(a-1)/max(b,.00001); }
)HLSL" + RoomCreatureModelsHlsl() + R"HLSL(
float3 LifeLocal(float3 p,uint i)
{
    float3 q=(p-lifeObjects[i*4].xyz)/lifeObjects[i*4].w;
    if((int)lifeObjects[i*4+1].w==8) q.y-=.025*sin(lifeObjects[i*4+3].x*2.4);
    float3 forward=lifeObjects[i*4+1].xyz;
    return float3(dot(q,float3(forward.z,0,-forward.x)),q.y,dot(q,forward));
}
bool LifeSphere(float3 o,float3 d,float3 c,float r,out float a,out float b)
{
    float3 q=o-c; float v=dot(q,d), h=v*v-dot(q,q)+r*r;
    a=-v-sqrt(max(h,0)); b=-v+sqrt(max(h,0)); return h>=0 && b>0;
}
bool LifeTrace(float3 o,float3 d,float limit,out float nearest,out uint object)
{
    nearest=limit; object=0; bool found=false;
    [loop] for(uint i=0;i<lifeCount;++i)
    {
        float a,b; float4 instance=lifeObjects[i*4];
        if(!LifeSphere(o,d,instance.xyz,instance.w,a,b) || a>nearest) continue;
        float t=max(a,.001);
        [loop] for(int step=0;step<56 && t<min(b,nearest);++step)
        {
            float distance=LifeShape(LifeLocal(o+d*t,i),(int)lifeObjects[i*4+1].w,lifeObjects[i*4+2].w,lifeObjects[i*4+3].x)*instance.w;
            if(distance<.0008) { nearest=t; object=i; found=true; break; }
            t+=max(distance*.65,.0004);
        }
    }
    return found;
}
bool LifeBoxShadow(float3 o,float3 d,float3 lo,float3 hi,float limit)
{
    float3 a=(lo-o)/d,b=(hi-o)/d,nearT=min(a,b),farT=max(a,b);
    float enter=max(nearT.x,max(nearT.y,nearT.z)), leave=min(farT.x,min(farT.y,farT.z));
    return leave>max(enter,.002) && enter<limit;
}
float LifeVisibility(float3 p,float3 lamp)
{
    if((lifeFlags&32)==0) return 1;
    float3 delta=lamp-p; float distance=length(delta), t; uint object;
    float3 d=delta/max(distance,.001), start=p+d*.008;
    if(LifeTrace(start,d,distance-.015,t,object)) return .08;
#if ROOM_STANDS
    float3 lo,hi;
    // Restrict shadow tests to rows and seats crossed by the ray's x/z bounds.
    [loop] for(uint row=0;row<standsRows;++row)
    {
        float z=standsZ0+row*standsPitch;
        if(z<min(p.z,lamp.z)-1 || z>max(p.z,lamp.z)+1) continue;
        RowBlockH((int)row,lo,hi);
        if(LifeBoxShadow(start,d,lo,hi,distance-.015)) return .08;
        [loop] for(uint col=0;col<seatCols;++col)
        {
            float x=seatX0+(seatJMin+(int)col)*ROOM_SEAT_PITCH;
            if(x<min(p.x,lamp.x)-.6 || x>max(p.x,lamp.x)+.6) continue;
            [loop] for(int box=0;box<4;++box)
            {
                SeatPartH(x,standsT0+row*standsRiser,z,box,lo,hi);
                if(box>0) hi.y+=box==1?ROOM_BACK_R:ROOM_ARM_R;
                if(LifeBoxShadow(start,d,lo,hi,distance-.015)) return .08;
            }
        }
    }
#endif
    return 1;
}
float3 LifeDisco(float3 p,float3 n)
{
    if((lifeFlags&16)==0) return 0;
    float3 result=0;
    [loop] for(int i=0;i<3;++i)
    {
        float a=lifeTime*.22+i*2.094;
        float3 lamp=float3(cos(a)*X*.65,yC-.16,zB*.58+sin(a)*.5);
        float3 target=float3(sin(a*1.3)*X,yF,lerp(FrontDepthR(0)+.5,zB-.5,.5+.4*cos(a*.8)));
        float3 axis=normalize(target-lamp), from=p-lamp;
        float cone=smoothstep(.87,.96,dot(normalize(from),axis));
        float amount=cone*max(0,dot(n,-normalize(from)))*(1+audioBass*2)/(1+dot(from,from)*.08);
        if(amount>.005) result+=LifePalette(i/3.0+lifeTime*.025)*amount*LifeVisibility(p,lamp)*lifeGain;
    }
    return result;
}
float3 LifeVisualLayer(float3 p,int face,float footprint,uint preset)
{
    if((lifeFlags&2)==0 || face==ROOM_FACE_FLOOR) return 0;
    float2 q=face==ROOM_FACE_CEILING?p.xz:face==ROOM_FACE_LEFT || face==ROOM_FACE_RIGHT?p.zy:p.xy;
    float t=lifeTime, r=length(q), angle=atan2(q.y,q.x), energy=saturate(audioLevel*visualReactivity);
    float width=max(.025,footprint*1.4), value=0, hue=0;
    float bass=saturate(audioBass*visualReactivity), mid=saturate(audioMid*visualReactivity);
    if(preset==0) { float x=frac(q.x*.18); uint bin=min(15u,(uint)(x*16)); float h=saturate(lifeSpectrum[bin/4][bin%4]*visualReactivity); value=(1-smoothstep(.03,.07,abs(frac(q.x*2)-.5)))*(1-smoothstep(h*2.5-.03,h*2.5+.03,abs(q.y))); hue=x; }
    if(preset==1) { float sample=frac(q.x*.18)*47; uint a=(uint)sample, b=min(a+1,47u);
        float wave=lerp(lifeWaveform[a/4][a%4],lifeWaveform[b/4][b%4],frac(sample))*.8*visualReactivity; value=1-smoothstep(width,width*3,abs(q.y-wave)); hue=q.x*.1+t*.03; }
    if(preset==2) { value=.5+.5*sin(q.x*2+sin(q.y*3+t)+t+mid*4); value*=.5+.5*sin(q.y*2-t); hue=value+t*.02; }
    if(preset==3) { value=pow(.5+.5*cos(r*7-t*2-bass*6),8); hue=r*.15-t*.04; }
    if(preset==4) { value=pow(.5+.5*cos(angle*6+sin(r*3-t)*3),10); hue=angle/6.283+t*.03; }
    if(preset==5) { float2 grid=abs(frac(q*.8+float2(0,t*.12))-.5); value=1-smoothstep(width,width*2,min(grid.x,grid.y)); hue=q.y*.1+t*.02; }
    if(preset==6) { value=pow(.5+.5*sin(q.y*3+sin(q.x*.7+t*.2)*3+t),5); hue=.3+q.x*.05+t*.02; }
    if(preset==7) { value=pow(.5+.5*cos(angle*9+r*4-t*1.5),14); hue=angle/6.283+r*.05; }
    if(preset==8) { float a=sin(q.x*1.3+t)*cos(q.y*1.7-t*.6); value=pow(abs(a),4); hue=a*.4+t*.02; }
    if(preset==9) { float2 cell=floor(q*2); float seed=LifeHash(dot(cell,float2(13,71))); value=pow(max(0,1-length(frac(q*2)-.5)*2),5)*(.3+.7*sin(t+seed*6.28)*sin(t+seed*6.28)); hue=seed+t*.02; }
    if(preset==10) { value=pow(.5+.5*cos(q.x*3+sin(q.y*2+t)*2+t),12); hue=q.y*.1+t*.02; }
    if(preset==11) { float wave=sin(q.x*2+t)*sin(q.y*2-t); value=pow(abs(wave),6); hue=wave+t*.04; }
    // Silence leaves a faint idle animation; audio drives amplitude and geometry.
    return LifePalette(hue)*value*(.08+energy*.8+bass*.35)*lifeGain;
}
float3 LifeVisual(float3 p,int face,float footprint)
{
 float3 colour=0;float layers=0;
 [loop] for(uint preset=0;preset<12;++preset) if((visualLayers&(1u<<preset))!=0)
 {colour+=LifeVisualLayer(p,face,footprint,preset);layers+=1;}
 return colour/sqrt(max(1,layers));
}
)HLSL" + VrexBrandingHlsl() + R"HLSL(
float3 LifeCompose(float3 base,int kind,float2 uv,float2 guv,uint e,float3 o,float3 d,float3 Dx,float3 Dy)
{
    if(lifeFlags==0 || !InsideH(o)) return base;
    float3 ray=normalize(d), p=0, n=0; float limit=1000;
    bool surface=kind>=1 && kind<=ROOM_FACES;
    if(surface) { FacePointH(kind-1,uv.x,uv.y,p,n); limit=length(p-o); }
#if ROOM_STANDS
    if(kind>=ROOM_KIND_STANDS) { limit=uv.x*length(d); p=o+ray*limit; int normal=(kind-ROOM_KIND_STANDS)%6, ax=normal/2; float sg=normal%2==0?1:-1; n=ax==0?float3(sg,0,0):ax==1?float3(0,sg,0):float3(0,0,sg); surface=true; }
#endif
    // The screen occludes creatures behind it; those in front share its projection
    // layer, including for a flat screen, so they have correct stereo occlusion.
    if(kind==ROOM_KIND_FOOTPRINT || kind==ROOM_KIND_OUTSIDE) return base;
    if(kind==0)
    {
        if(R>0) { float3 hit; if(!RoomArcRoot(R,R,scrTanWrap,halfH,o,ray,limit,hit)) return base; }
        else limit=-o.z/ray.z;
    }
    float3 light=Dec(base);
    if(surface)
    {
        light+=LifeDisco(p,n);
        if(kind<=ROOM_FACES) light+=LifeVisual(p,kind-1,length(Dx+Dy)*limit);
    }
    float t; uint object;
    if(LifeTrace(o,ray,limit,t,object))
    {
        float3 hit=o+ray*t, local=LifeLocal(hit,object);
        int k=(int)lifeObjects[object*4+1].w; float phase=lifeObjects[object*4+2].w;
        float eps=.004;
        float3 normal=normalize(float3(LifeShape(local+float3(eps,0,0),k,phase,lifeObjects[object*4+3].x)-LifeShape(local-float3(eps,0,0),k,phase,lifeObjects[object*4+3].x),LifeShape(local+float3(0,eps,0),k,phase,lifeObjects[object*4+3].x)-LifeShape(local-float3(0,eps,0),k,phase,lifeObjects[object*4+3].x),LifeShape(local+float3(0,0,eps),k,phase,lifeObjects[object*4+3].x)-LifeShape(local-float3(0,0,eps),k,phase,lifeObjects[object*4+3].x)));
        float3 forward=lifeObjects[object*4+1].xyz;
        float3 wn=normal.x*float3(forward.z,0,-forward.x)+normal.y*float3(0,1,0)+normal.z*forward;
        float3 colour=LifePalette(phase*.159+local.z*.18+audioMid*.15);
        float shade=.24+.55*max(0,dot(wn,normalize(float3(-.4,.8,.3))));
        float rim=pow(1-saturate(dot(wn,-ray)),3);
        float scan=.65+.35*sin(local.y*65-lifeTime*4);
        float3 body=colour*(shade+rim*.9)*(k==0 || k==3 || k==4?scan:1);
        if(k==0 || k==1)
        {
            // Eyes and gills make even the smallest fish readable from either side.
            float eye=length(float2(abs(local.x)-.145,local.y-.055));
            if(local.z>.43 && local.z<.56 && eye<.07) body=float3(.06,.9,1)*(1+rim);
            if(k==0 && local.z>-.05 && local.z<.22 && abs(local.x)>.14 && abs(frac(local.z*18)-.5)<.13) body*=.30;
            body+=colour*rim*.25*lifeBeat;
        }
        float highlight=pow(saturate(dot(reflect(ray,wn),normalize(float3(-.4,.8,.3)))),28);
        float attention=lifeObjects[object*4+3].y;
        if(k==0 || k==1)
        {
            float scales=sin(local.z*75+sin(local.y*40))*sin(local.y*46)*.045;
            body*=1+scales;
            if(local.y<-.035) body=lerp(body,float3(.50,.73,.80)*(shade+.15),.55);
            body+=highlight*float3(.35,.7,.95);
        }
        if(k==2 || k==8)
        {
            bool hero=k==8;
            float3 metal=hero?float3(.70,.82,.88):float3(.16,.27,.38);
            float brushed=.96+.04*sin(local.y*220+local.x*70);
            body=metal*(shade+.12)*brushed+float3(.5,.75,1)*highlight*.6;
            // Dark flexible joint seals, boot soles, panel breaks and the face lens.
            if(local.y<-.34 || abs(local.x)>.31) body*=.42;
            if(abs(local.y+.30)<.025 || (abs(abs(local.x)-.25)<.012 && local.y<.19)) body*=.45;
            if(local.y>.33 && local.y<.64 && local.z>.24)
            {
                body=float3(.012,.028,.05)+highlight*.20;
                float blink=frac(lifeTime*.19+phase*.13)>.965?.10:1;
                float eye=length((float2(abs(local.x)-.125,local.y-.49))/float2(.060,.070*blink));
                if(eye<1) body=lerp(float3(.04,.7,1),float3(.6,1,.65),attention)*(1+.25*audioBass);
                float smile=abs(local.y-(.405+local.x*local.x*.7));
                if(abs(local.x)<.11 && smile<.008) body=float3(.1,.7,.95);
            }
            if(hero && local.z>.24 && abs(local.x)<.235 && local.y>-.22 && local.y<.18)
                body=VrexBadge(float2(.5+local.x/.47,.5-(local.y+.02)/.40))*(.6+shade);
            if(abs(local.x)>.32 && abs(local.y-.48)<.04) body=float3(.03,.55,.9)*(1+attention);
            if(local.y>.82) body=float3(.9,.35,.03)*(1+.5*sin(lifeTime*3));
            if(hero && local.z<-.22 && local.y<-.24 && vrexState!=2)
                body=float3(.08,.55,1)*(1.4+.4*sin(lifeTime*35));
            // Small recessed fasteners along the chest plate.
            float2 bolt=float2(abs(local.x)-.215,abs(local.y+.02)-.17);
            if(local.z>.22 && length(bolt)<.018) body=float3(.035,.055,.075)+highlight*.5;
        }
        if(k==8)
        {
            float3 teal=float3(.025,.58,.49);
            body=teal*(shade+.28)+highlight*float3(.40,.9,.80)*.55;
            if(local.y<-.62) body=float3(.025,.17,.15)*(shade+.2)+highlight*.15;
            if(local.z>.26 && local.y>.08 && local.y<.60 && abs(local.x)<.34)
            {
                body=float3(.009,.019,.045)+highlight*.16;
                float blink=frac(lifeTime*.17+phase*.1)>.97?.12:1;
                if((((uint)vrexState>>8)&3)==1 && local.x<0 && frac(lifeTime*1.1)<.32) blink=.12;
                float eyeX=(abs(local.x)-.145)/.078;
                float eyeY=.40+.050*(1-eyeX*eyeX)*blink;
                if(abs(eyeX)<1 && abs(local.y-eyeY)<.016) body=float3(.30,.95,.82)*(1+.3*attention);
                float smile=.20+(1.2+.35*saturate(vrexChat)) *local.x*local.x;
                if(abs(local.x)<.105 && abs(local.y-smile)<.013) body=float3(.28,.92,.80);
                if(length(float2(abs(local.x)-.235,local.y-.235))<.034) body=float3(.58,.17,.36);
            }
            if(local.y>.81) body=float3(.30,1,.82)*1.3;
            if(local.z>.19 && abs(local.x)<.185 && local.y>-.47 && local.y<-.10)
                body=VrexBadge(float2(.5+local.x/.37,.5-(local.y+.285)/.37))*(.6+shade);
            if(local.z<-.18 && local.y<-.42) body=float3(.08,.65,.9)*(1.2+.3*sin(lifeTime*30));
        }
        if(k==3)
        {
            float veins=pow(.5+.5*cos(atan2(local.z,local.x)*8+local.y*3),12);
            body=colour*(.20+rim*1.1+veins*.4)+highlight*.5;
            if(local.y<.08) body=LifePalette(local.y*.8+phase*.1)*(.6+audioHigh*.6);
        }
        if(k==4)
        {
            float spots=pow(saturate(sin(local.x*43)*sin(local.z*37)),14);
            body=lerp(float3(.05,.16,.24),colour*.65,.3)*(shade+.3)+spots*.6+rim*colour;
            if(local.y<-.04) body=float3(.60,.77,.80)*(shade+.1);
        }
        if(k==6)
        {
            float2 shell=local.xz*9; shell.x+=floor(shell.y)*.5;
            float seam=smoothstep(.40,.48,max(abs(frac(shell.x)-.5),abs(frac(shell.y)-.5)));
            body=local.y>-.42?lerp(float3(.36,.18,.045),float3(.10,.19,.055),seam):float3(.12,.42,.15);
            body*=shade+.25;body+=highlight*.18;
            if(local.z>.64 && abs(abs(local.x)-.12)<.04 && local.y>-.47 && local.y<-.36) body=float3(.015,.025,.01)+highlight*.3;
        }
        if(k==5)
        {
            body=local.z<0?colour*(1.2+audioHigh*1.3):float3(.10,.20,.13)*(shade+.2);
            if(abs(local.x)>.25) body=colour*(.35+rim);
        }
        if(k==7)
        {
            body=float3(.14,.22,.29)*(shade+.1)+highlight*.6;
            if(local.y>.06 && abs(local.x)<.21 && abs(local.z)<.21) body*=.5;
            if(abs(local.x)>.37 || abs(local.z)>.37) body+=colour*(.12+rim*.6);
            if(local.z>.18 && local.y<-.12) body=float3(.02,.65,.85)*(1+attention);
        }
        body+=LifeDisco(hit,wn)*.5;
        light=body*lifeGain;
    }
    if((lifeFlags&8)!=0)
    {
        // Stable world-space motes with real depth, soft sphere edges and stereo parallax.
        [loop] for(uint i=0;i<(uint)particleDensity;++i)
        {
            float seed=i+120.0;
            float3 c=float3((LifeHash(seed)*2-1)*(X-.1),lerp(yF+.1,yC-.1,frac(LifeHash(seed+20)+lifeTime*.006)),lerp(FrontDepthR(0)+.15,zB-.15,LifeHash(seed+50)));
            c.x+=sin(lifeTime*.15+seed)*.045; c.z+=cos(lifeTime*.11+seed)*.045;
            if(c.z<FrontDepthR(c.x)+.05) continue;
            float a,b, radius=.009+LifeHash(seed+80)*.009;
            if(LifeSphere(o,ray,c,radius,a,b) && a>0 && a<min(limit,t))
            {
                float alpha=saturate((b-a)/(radius*2));
                light=lerp(light,LifePalette(LifeHash(seed+5)+lifeTime*.008)*(.4+audioHigh)*lifeGain,alpha*.8);
            }
        }
    }
    light=VrexMenu(light,o,ray,min(limit,t));
    light=VrexBubble(light,o,ray,min(limit,t));
    return Enc(max(light,0));
}
)HLSL"; }
