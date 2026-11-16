#pragma once
#include "room.h"
#include "room_audio.h"

enum LifeKind { LifeShark, LifeFish, LifeRobot, LifeJelly, LifeManta, LifeFirefly, LifeTurtle, LifeDrone, LifeVrex, LifeKinds };
enum LifeAttitude { Ignore, School, Follow, Flee, Tease, Escort, Guard, Investigate, Chase };
// Directed, like Eloria: rows are the observer, columns the observed creature.
//                 shark   fish    robot   jelly   manta   firefly turtle  drone
inline constexpr LifeAttitude kLifeRelations[8][8] = {
    { School, Chase, Ignore, Investigate, Escort, Ignore, Ignore, Investigate },
    { Flee, School, Investigate, School, Flee, Follow, School, Flee },
    { Investigate, Guard, School, Investigate, Ignore, Follow, Escort, Escort },
    { Ignore, School, Investigate, School, Escort, School, Guard, Investigate },
    { Escort, Guard, Ignore, Follow, School, Investigate, Escort, Investigate },
    { Flee, School, Tease, School, Flee, School, Follow, Tease },
    { Flee, Follow, Escort, Investigate, Ignore, Investigate, School, Guard },
    { Investigate, Chase, Escort, Follow, Investigate, Follow, Guard, School }
};
inline constexpr float kLifeRadius[9] = { .52f,.13f,.23f,.23f,.45f,.07f,.25f,.21f,.14f };
inline constexpr float kLifeSpeed[9] = { .48f,.42f,.22f,.16f,.36f,.32f,.12f,.38f,.65f };
struct RoomLifeSettings
{
    int flags = 0, preset = 0, species = 255, count = 12, gain = 100, density = 40;
    int layers = -1, reactivity = 100, beams = 1, vrex = 0;
};
struct LifeVec
{
    float x=0,y=0,z=0;
    LifeVec operator+(LifeVec b) const { return {x+b.x,y+b.y,z+b.z}; }
    LifeVec operator-(LifeVec b) const { return {x-b.x,y-b.y,z-b.z}; }
    LifeVec operator*(float k) const { return {x*k,y*k,z*k}; }
    float Length() const { return sqrtf(x*x+y*y+z*z); }
};
struct LifeCreature
{
    LifeVec p, v; int kind=0; float phase=0;
    float heading=.785398f, animation=0, attention=0;
    LifeVec beamTarget;
    bool startled=false;
};
struct LifeBeam { LifeVec origin, direction; float limit=20; bool active=false, trigger=false; };
struct LifePlayer { LifeVec head, forward{0,0,-1}; bool valid=false; LifeBeam beams[2]; };
struct VrexMenuFrame { LifeVec centre, right, up, normal; };
inline float LifeDot(LifeVec a,LifeVec b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
inline VrexMenuFrame MakeVrexMenuFrame(LifeVec head,LifeVec pal)
{
    LifeVec n=head-pal;float distance=n.Length();n=distance>.001f?n*(1/distance):LifeVec{0,0,1};
    LifeVec right{n.z,0,-n.x};float width=right.Length();right=width>.001f?right*(1/width):LifeVec{1,0,0};
    LifeVec up{n.y*right.z-n.z*right.y,n.z*right.x-n.x*right.z,n.x*right.y-n.y*right.x};
    return {pal+right*.36f+up*.10f,right,up,n};
}
enum VrexMode { VrexOrbit, VrexLanding, VrexPerched, VrexTakeoff, VrexWave, VrexBeamFollow };


class RoomLife
{
    std::array<LifeCreature,24> creatures{};
    int count=0, oldMask=-1, oldWanted=-1, audioStatus=-99;
    float clock=0, accumulator=0, screenW=0, screenH=0;
    uint32_t random=0x58435245;
    RoomAudio audio;
    RoomAudioFrame smooth;
    LifePlayer player;
    LifeCreature pal;
    bool palActive=false;
    VrexMode palMode=VrexOrbit;
    float palTimer=0, palChatTimer=0;
    int palChat=0, palHand=-1;
    float palMenuTimer=0, palHoverTime=0, palReplyTimer=0, palGestureTimer=0;
    int palMenuHover=0, palMenuPressed=0, palGesture=0, palMenuHand=-1;
    bool palMenuArmed=false;
    LifeVec palAnchor;
    float Rand() { random ^= random<<13; random ^= random>>17; random ^= random<<5; return (random & 0xFFFFFF)/16777216.f; }
    static bool Ground(int k) { return k==LifeRobot || k==LifeTurtle; }
    static float Floor(const Room& r, LifeVec p)
    {
        float floor=r.yF;
        for(int k=0;k<r.standsRows;++k)
            if(p.z>=r.standsZ0+(k-.5f)*r.standsPitch) floor=std::max(floor,r.standsT0+k*r.standsRiser);
        return floor;
    }
    static float GroundY(const Room& r,LifeVec p,float radius)
    {
        float y=Floor(r,p)+radius;
        for(int row=0;row<r.standsRows;++row)
        {
            float edge=r.standsZ0+(row-.5f)*r.standsPitch, gap=edge-p.z;
            if(gap>0 && gap<radius)
                y=std::max(y,r.standsT0+row*r.standsRiser+sqrtf(std::max(0.f,radius*radius-gap*gap)));
        }
        return y+.002f;
    }
public:
    // Conservative spheres enclose every animated part. Inflated furniture boxes
    // include the rounded caps; visual meshes may approach but cannot interpenetrate.
    static void ResolveContact(LifeCreature& a,LifeCreature& b)
    {
        LifeVec separation=a.p-b.p; float distance=separation.Length();
        if(distance<.00001f) return;
        LifeVec n=separation*(1/distance), relative=a.v-b.v;
        float closing=relative.x*n.x+relative.y*n.y+relative.z*n.z;
        if(closing>=0) return;
        float ra=kLifeRadius[a.kind],rb=kLifeRadius[b.kind];
        float inverseA=1/(ra*ra*ra),inverseB=1/(rb*rb*rb);
        float impulse=-(1+.35f)*closing/(inverseA+inverseB);
        a.v=a.v+n*(impulse*inverseA); b.v=b.v-n*(impulse*inverseB);
        if(Ground(a.kind)) a.v.y=0; if(Ground(b.kind)) b.v.y=0;
    }
    static bool Clear(const Room& r, LifeVec p, float radius)
    {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !(radius>0)) return false;
        if (!r.valid || fabsf(p.x)+radius>=r.X || p.y-radius<Floor(r,p)-.001f || p.y+radius>=r.yC || p.z+radius>=r.zB) return false;
        for(int i=0;i<3;++i)
        {
            float x=p.x+radius*(i-1);
            float front=FrontDepth(r,x);
            if(p.z-radius<=front) return false;
        }
        RoomStands s=MakeRoomStands(r);
        for(int k=0;k<s.rows;++k)
        {
            float lo[3],hi[3]; RoomRowBlock(s,k,lo,hi);
            float distance=0; float q[3]={p.x,p.y,p.z};
            for(int a=0;a<3;++a) { float v=q[a]-std::clamp(q[a],lo[a],hi[a]); distance+=v*v; }
            if(distance<radius*radius-.00001f) return false;
        }
        for(int k=0;k<s.rows;++k) for(int j=s.jMin;j<s.jMin+s.cols;++j) for(int b=0;b<4;++b)
        {
            float lo[3],hi[3];
            RoomSeatPart(s.x0+j*kRoomSeatPitch,s.t0+k*s.riser,s.z0+k*s.pitch,b,lo,hi);
            if(b>0) hi[1]+=b==1?kRoomBackR:kRoomArmR;
            float d=0; float q[3]={p.x,p.y,p.z};
            for(int a=0;a<3;++a) { float v=q[a]-std::clamp(q[a],lo[a],hi[a]); d+=v*v; }
            if(d<radius*radius) return false;
        }
        return true;
    }
    bool Free(const Room& r, LifeVec p, float radius, int skip=-1) const
    {
        if(!Clear(r,p,radius)) return false;
        if(player.valid && (p-player.head).Length()<radius+.16f) return false;
        if(screenW>0 && fabsf(p.y)<screenH*.5f+radius)
        {
            if(!r.curved && fabsf(p.x)<screenW*.5f+radius && fabsf(p.z)<radius+.025f) return false;
            if(r.curved)
            {
                float rho=sqrtf(p.x*p.x+(r.R-p.z)*(r.R-p.z));
                float arc=r.R*atan2f(p.x,r.R-p.z);
                if(fabsf(rho-r.R)<radius+.025f && fabsf(arc)<screenW*.5f+radius) return false;
            }
        }
        for(int j=0;j<count;++j) if(j!=skip && (p-creatures[j].p).Length()<radius+kLifeRadius[creatures[j].kind]+.015f) return false;
        if(palActive && skip!=24 && (p-pal.p).Length()<radius+kLifeRadius[LifeVrex]+.015f) return false;
        return true;
    }
    void Populate(const Room& r, const RoomLifeSettings& settings)
    {
        count=0;
        int kinds[8], n=0;
        for(int k=0;k<8;++k) if(settings.species&(1<<k)) kinds[n++]=k;
        if(!n) return;
        for(int i=0;i<std::clamp(settings.count,0,24);++i)
        {
            int kind=kinds[i%n]; float radius=kLifeRadius[kind];
            for(int attempt=0;attempt<400;++attempt)
            {
                LifeVec p{(Rand()*2-1)*(r.X-radius-.03f),0,0};
                float front=FrontDepth(r,p.x)+radius+.04f;
                p.z=front+Rand()*std::max(0.f,r.zB-radius-.04f-front);
                float floor=GroundY(r,p,radius);
                p.y=Ground(kind)?floor:floor+Rand()*std::max(0.f,r.yC-radius-.03f-floor);
                if(Free(r,p,radius)) { creatures[count++]={p,{.1f,0,.1f},kind,Rand()*6.2831853f}; break; }
            }
        }
    }
    void Step(const Room& r, float dt, float music)
    {
        // Sequential collision-safe integration. A fixed 120 Hz step bounds travel
        // to < 1 cm; rejected full moves try each axis to slide around furniture.
        for(int i=0;i<count;++i)
        {
            auto& c=creatures[i]; const LifeVec start=c.p; float radius=kLifeRadius[c.kind];
            LifeVec force{cosf(clock*.29f+c.phase),sinf(clock*.37f+c.phase)*.4f,sinf(clock*.23f+c.phase)};
            for(int j=0;j<count;++j) if(j!=i)
            {
                auto& other=creatures[j]; LifeVec delta=other.p-c.p; float d=delta.Length();
                if(d<.0001f) continue;
                LifeVec dir=delta*(1/d); float safe=radius+kLifeRadius[other.kind]+.25f;
                if(d<safe) force=force-dir*(4*(safe-d)/safe);
                if(d<2.5f)
                {
                    auto relation=kLifeRelations[c.kind][other.kind];
                    if(relation==School) force=force+other.v*.7f+dir*(d>safe+.2f?.6f:-.6f);
                    if(relation==Follow) force=force+dir*(d>safe+.2f?.85f:-.5f);
                    if(relation==Flee) force=force-dir*(2.5f-d)*2;
                    if(relation==Tease) force=force+LifeVec{-dir.z,.1f,dir.x}*.9f+dir*(d>safe+.4f?.4f:-1.f);
                    if(relation==Escort) force=force+other.v*.9f+dir*(d>safe+.55f?.7f:-.4f);
                    if(relation==Guard) { float threat=0;for(int k=0;k<count;++k) if(creatures[k].kind==LifeShark) {LifeVec away=other.p-creatures[k].p;float l=away.Length();if(l<2 && l>.001f) {LifeVec intercept=other.p-away*.25f-c.p;float gap=intercept.Length();if(gap>.001f) force=force+intercept*(.6f/gap);threat=1;}} force=force+dir*(d>safe+.7f?.5f:-.3f)*(1+threat); }
                    if(relation==Investigate) force=force+dir*(d>safe+.35f?.35f:-.5f)*(.5f+.5f*sinf(clock*.5f+c.phase));
                    if(relation==Chase) force=force+dir*(d>safe+.3f?1.05f:-1.2f);

                }
            }
            if(c.attention>.05f)
            {
                LifeVec to=c.beamTarget-c.p;float d=to.Length();
                if(d>.001f)
                {
                    float sign=c.startled && (c.kind==LifeFish || c.kind==LifeShark || c.kind==LifeManta)?-1.f:1.f;
                    force=force+to*(sign*c.attention*2.5f/d);
                    if(c.kind==LifeFirefly) force=force+LifeVec{-to.z,0,to.x}*(c.attention*2);
                }
                if(c.kind==LifeTurtle && c.startled) force=force-c.v*5;
            }
            c.v=c.v+force*dt;
            float speed=kLifeSpeed[c.kind]*(1+music*.7f), len=c.v.Length();
            if(len>speed) c.v=c.v*(speed/len);
            if(Ground(c.kind)) c.v.y=0;
            float targetHeading=atan2f(c.v.x,c.v.z);
            c.heading+=std::remainder(targetHeading-c.heading,6.2831853f)*(1-expf(-dt*4));
            c.animation+=dt*(.3f+2.5f*c.v.Length()/std::max(.1f,kLifeSpeed[c.kind]))*(1+music*.3f);
            LifeVec next=c.p+c.v*dt;
            if(Ground(c.kind)) next.y=std::clamp(GroundY(r,next,radius),start.y-.55f*dt,start.y+.55f*dt);
            if(Free(r,next,radius,i)) c.p=next;
            else
            {
                for(int j=0;j<count;++j) if(j!=i && (next-creatures[j].p).Length()<radius+kLifeRadius[creatures[j].kind]+.02f)
                    ResolveContact(c,creatures[j]);
                c.v=c.v*.92f;
                for(int a=0;a<3;++a)
                {
                    LifeVec slide=c.p;
                    if(a==0) slide.x=next.x; if(a==1) slide.y=next.y; if(a==2) slide.z=next.z;
                    if(Ground(c.kind)) slide.y=std::clamp(GroundY(r,slide,radius),start.y-.55f*dt,start.y+.55f*dt);
                    if(Free(r,slide,radius,i)) c.p=slide;
                    else if(!Clear(r,slide,radius)) { if(a==0) c.v.x*=-.5f; if(a==1) c.v.y*=-.5f; if(a==2) c.v.z*=-.5f; }
                }
                c.v=c.v+LifeVec{-c.v.z,0,c.v.x}*dt*3;
            }
        }
    }
    void SetPlayer(const LifePlayer& input) { player=input; }
    bool GetVrexInteraction(LifeVec& position,VrexMenuFrame& menu,int& host,bool& open) const
    {
        position=pal.p;menu=MakeVrexMenuFrame(player.head,pal.p);host=palHand;open=palMenuTimer>0;
        return palActive && player.valid;
    }
    static int VrexMenuHit(const VrexMenuFrame& menu,const LifeBeam& beam,float limit)
    {
        float denom=LifeDot(beam.direction,menu.normal);if(fabsf(denom)<.0001f) return 0;
        float t=LifeDot(menu.centre-beam.origin,menu.normal)/denom;
        if(t<=0 || t>limit) return 0;
        LifeVec p=beam.origin+beam.direction*t-menu.centre;
        float x=LifeDot(p,menu.right), y=LifeDot(p,menu.up);
        if(fabsf(x)>.24f || fabsf(y)>.10f || fabsf(y)<.006f) return 0;
        return y>0?1:2;
    }
    void UpdateVrexMenu(const Room& r,float dt,bool enabled)
    {
        palMenuTimer=std::max(0.f,palMenuTimer-dt);palMenuHover=0;
        if(!enabled || !palActive || palHand<0 || !player.beams[1-palHand].active)
        {palMenuTimer=palHoverTime=0;palMenuPressed=0;palMenuHand=-1;palMenuArmed=false;return;}
        int hand=1-palHand;const auto& beam=player.beams[hand];
        if(hand!=palMenuHand) {palMenuHand=hand;palHoverTime=0;palMenuPressed=0;palMenuArmed=false;}
        float limit=beam.limit;
        float o[3]{beam.origin.x,beam.origin.y,beam.origin.z}, d[3]{beam.direction.x,beam.direction.y,beam.direction.z};
        RoomHit wall;if(RoomExit(r,o,d,wall)) limit=std::min(limit,wall.t);
        float inv[3]{1/d[0],1/d[1],1/d[2]};RoomStandsHit seat;
        if(RoomStandsTrace(MakeRoomStands(r),o,d,inv,limit,seat)) limit=std::min(limit,seat.t);
        LifeVec to=pal.p-beam.origin;float along=LifeDot(to,beam.direction);
        bool nearPal=along>0 && along<limit && (to-beam.direction*along).Length()<.22f;
        palHoverTime=nearPal?palHoverTime+dt:0;
        if(palHoverTime>=.18f) palMenuTimer=1.4f;
        if(palMenuTimer>0)
        {
            palMenuHover=VrexMenuHit(MakeVrexMenuFrame(player.head,pal.p),beam,limit);
            if(palMenuHover) palMenuTimer=1.4f;
            if(beam.trigger && palMenuArmed)
            {palMenuArmed=false;palMenuPressed=palMenuHover;}
            if(!beam.trigger && palMenuPressed)
            {
                if(palMenuHover==palMenuPressed)
                {
                    palChat=palMenuPressed==1?0:6;palChatTimer=4;palReplyTimer=4;
                    palGesture=palMenuPressed;palGestureTimer=2.8f;
                }
                palMenuPressed=0;
            }
        }
        if(!beam.trigger) palMenuArmed=true;
        if(palMenuTimer<=0) palMenuPressed=0;
    }
    void ReactToBeams(const Room& r,float dt,bool enabled)
    {
        for(auto& c:creatures) c.attention*=expf(-dt*3);
        pal.attention*=expf(-dt*3);
        if(!enabled || !player.valid) return;
        for(const auto& beam:player.beams) if(beam.active)
        {
            float best=beam.limit; int picked=-1;
            float o[3]={beam.origin.x,beam.origin.y,beam.origin.z},d[3]={beam.direction.x,beam.direction.y,beam.direction.z};
            RoomHit wall; if(RoomExit(r,o,d,wall)) best=std::min(best,wall.t);
            float inv[3]={1/d[0],1/d[1],1/d[2]}; RoomStandsHit seat;
            if(RoomStandsTrace(MakeRoomStands(r),o,d,inv,best,seat)) best=std::min(best,seat.t);
            for(int i=0;i<count+(palActive?1:0);++i)
            {
                auto& c=i<count?creatures[i]:pal;
                LifeVec q=c.p-beam.origin;float along=q.x*d[0]+q.y*d[1]+q.z*d[2];
                float radius=kLifeRadius[c.kind]+.035f;
                float square=q.x*q.x+q.y*q.y+q.z*q.z-along*along;
                if(square<radius*radius && along>0)
                {
                    float t=along-sqrtf(radius*radius-square);
                    if(t>=0 && t<best) { best=t;picked=i; }
                }
            }
            if(picked<0) continue;
            auto& c=picked<count?creatures[picked]:pal;
            c.attention=std::min(1.f,c.attention+dt*7);
            c.startled=beam.trigger;
            c.beamTarget=beam.origin+beam.direction*std::max(.5f,best-.4f);
            if(picked==count)
            {
                if(palReplyTimer<=0 && palMenuTimer<=0) {palChat=beam.trigger?3:0;palChatTimer=3;}
            }
        }
    }
    void UpdateVrex(const Room& r,float dt,bool enabled,bool reset)
    {
        if(!enabled || !player.valid) {palActive=false;palHand=-1;return;}
        LifeVec forward=player.forward;forward.y=0;float fl=forward.Length();forward=fl>.001f?forward*(1/fl):LifeVec{0,0,-1};
        LifeVec right{-forward.z,0,forward.x};
        LifeVec fallback=player.head+forward*.75f+right*.35f+LifeVec{0,-.12f,0};
        palChatTimer=std::max(0.f,palChatTimer-dt);
        palReplyTimer=std::max(0.f,palReplyTimer-dt);
        palGestureTimer=std::max(0.f,palGestureTimer-dt);
        if(palGestureTimer<=0) palGesture=0;
        LifeVec reference=palActive?pal.p:fallback, target=fallback;
        LifeVec anchors[2];bool available[2]{};float costs[2]={1e9f,1e9f};
        for(int hand=0;hand<2;++hand)
        {
            const auto& beam=player.beams[hand];
            float beamReach=std::min(1.35f,beam.limit-.18f);
            if(!beam.active || beamReach<.65f) continue;
            LifeVec to=reference-beam.origin;
            float along=std::clamp(to.x*beam.direction.x+to.y*beam.direction.y+to.z*beam.direction.z,.65f,beamReach);
            anchors[hand]=beam.origin+beam.direction*along+LifeVec{0,.14f,0};
            if(!Free(r,anchors[hand],kLifeRadius[LifeVrex],24)) continue;
            available[hand]=true;costs[hand]=(reference-anchors[hand]).Length();
        }
        int chosen=costs[0]<costs[1]?0:1;
        if(!available[chosen]) chosen=-1;
        // A small hysteresis keeps crossed beams from swapping hands every frame.
        if(palHand>=0 && available[palHand] && (chosen<0 || costs[palHand]<costs[chosen]+.12f)) chosen=palHand;
        if(palMenuTimer>0 && palHand>=0 && available[palHand]) chosen=palHand;
        if(chosen>=0) target=anchors[chosen];
        if(chosen!=palHand && chosen>=0 && pal.attention<.05f && palReplyTimer<=0) {palChat=1;palChatTimer=2;}
        palHand=chosen;palMode=chosen>=0?VrexBeamFollow:VrexOrbit;
        if(!palActive || reset)
        {
            palActive=false;pal={};pal.kind=LifeVrex;pal.phase=.4f;
            if(!Free(r,target,kLifeRadius[LifeVrex],24)) target=fallback;
            if(!Free(r,target,kLifeRadius[LifeVrex],24)) return;
            pal.p=target;palActive=true;palChat=0;palChatTimer=3;
        }
        if(!Free(r,target,kLifeRadius[LifeVrex],24)) target=pal.p;
        // A damped spring carries real momentum, so rapid hand movement leaves
        // him lagging, swinging past the beam and settling instead of teleporting.
        int steps=std::max(1,(int)ceilf(dt*120));float step=dt/steps;
        for(int tick=0;tick<steps;++tick)
        {
            LifeVec acceleration=(target-pal.p)*18.f-pal.v*4.5f;
            float force=acceleration.Length();if(force>10) acceleration=acceleration*(10/force);
            pal.v=pal.v+acceleration*step;float speed=pal.v.Length();if(speed>2.2f) pal.v=pal.v*(2.2f/speed);
            LifeVec next=pal.p+pal.v*step;
            if(Free(r,next,kLifeRadius[LifeVrex],24)) pal.p=next;
            else for(int axis=0;axis<3;++axis)
            {
                LifeVec slide=pal.p;
                if(axis==0) slide.x=next.x;if(axis==1) slide.y=next.y;if(axis==2) slide.z=next.z;
                if(Free(r,slide,kLifeRadius[LifeVrex],24)) pal.p=slide;
                else {if(axis==0) pal.v.x*=-.25f;if(axis==1) pal.v.y*=-.25f;if(axis==2) pal.v.z*=-.25f;}
            }
        }
        LifeVec look=player.head-pal.p;float angle=atan2f(look.x,look.z);
        float speed=pal.v.Length();
        if(speed>.35f && palMenuTimer<=0 && palGesture==0) angle=atan2f(pal.v.x,pal.v.z);
        // A slow, eased pirouette between interactions; returning to 2*pi is the
        // same orientation, so the end of the cycle never snaps.
        float turn=fmodf(clock,19.f)-15.f;
        if(turn>0 && turn<2.4f && palMenuTimer<=0 && speed<.15f && palGesture==0)
        {float q=turn/2.4f;angle+=6.2831853f*q*q*(3-2*q);}
        pal.heading+=std::remainder(angle-pal.heading,6.2831853f)*(1-expf(-dt*5));
        pal.animation+=dt;palAnchor=target;
        if(palChatTimer<=0 && (int)clock%13==0) {palChat=smooth.bass>.15f?2:5;palChatTimer=3;}
    }
    void EnableAudio(bool enabled) { audio.Enable(enabled); }
    void Fill(RoomConstants& rc, const Room& r, const RoomLifeSettings& s, float dt, bool reset)
    {
        screenW=rc.screenW; screenH=rc.screenH;
        audio.Enable((s.flags&1)!=0);
        int status=audio.Status();
        if(status!=audioStatus)
        {
            audioStatus=status;
            printf("Room audio: %s\n",status==2?"listening":status==1?"connecting":status<0?"unavailable":"off");
        }
        auto incoming=audio.Read();
        if(!(s.flags&1)) smooth={};
        float blend=1-expf(-std::clamp(dt,0.f,.1f)/.045f);
        auto ease=[blend](float& value,float target) { value+=(target-value)*blend; };
        ease(smooth.bass,incoming.bass); ease(smooth.mid,incoming.mid); ease(smooth.high,incoming.high); ease(smooth.level,incoming.level);
        smooth.beat=std::max(incoming.beat,smooth.beat*expf(-dt*6));
        for(int i=0;i<16;++i) ease(smooth.spectrum[i],incoming.spectrum[i]);
        auto sound=smooth;
        clock+=std::clamp(dt,0.f,.1f);
        if(reset) {palActive=false;palMenuTimer=palHoverTime=palReplyTimer=palGestureTimer=0;palGesture=palMenuPressed=palMenuHover=0;palMenuHand=-1;palMenuArmed=false;}
        if((s.flags&4) && (reset || oldMask!=s.species || oldWanted!=s.count || count==0)) { Populate(r,s); oldMask=s.species; oldWanted=s.count; }
        if(!(s.flags&4)) count=0;
        ReactToBeams(r,std::clamp(dt,0.f,.1f),s.beams!=0);
        UpdateVrex(r,std::clamp(dt,0.f,.1f),s.vrex!=0,reset);
        UpdateVrexMenu(r,std::clamp(dt,0.f,.1f),s.vrex!=0);
        if(s.flags&4)
        {
            accumulator+=std::clamp(dt,0.f,.1f);
            while(accumulator>=1.f/120) { Step(r,1.f/120,sound.bass); accumulator-=1.f/120; }
        }
        rc.lifeTime=clock; rc.audioBass=sound.bass; rc.audioMid=sound.mid; rc.audioHigh=sound.high;
        rc.lifeBeat=sound.beat; rc.audioLevel=sound.level; rc.lifeGain=s.gain*.01f; rc.particleDensity=(float)s.density;
        rc.lifePreset=s.preset; rc.lifeSpecies=s.species; rc.lifeFlags=s.flags|(s.vrex?128:0);
        rc.visualReactivity=s.reactivity*.01f; rc.visualLayers=s.layers<0?1u<<s.preset:(uint32_t)s.layers;
        int gesture=palGesture?palGesture:(pal.v.Length()>.35f && palMenuTimer<=0?3:0);
        rc.vrexChat=palChatTimer>0?(float)(palChat+1):0;
        // Packed into the existing float to retain the shared constant ABI:
        // mode[0:3], menu[4], hovered option[5:6], pressed[7], gesture[8:9].
        rc.vrexState=(float)((int)palMode+(palMenuTimer>0?16:0)+32*palMenuHover+(palMenuPressed?128:0)+256*gesture);
        rc.lifeCount=(s.flags&4)?count:0;
        unsigned visibleCreatures=rc.lifeCount;
        if(palActive) rc.lifeCount++;
        std::copy(sound.spectrum.begin(),sound.spectrum.end(),rc.lifeSpectrum);
        std::copy(incoming.waveform.begin(),incoming.waveform.end(),rc.lifeWaveform);
        for(unsigned i=0;i<rc.lifeCount;++i)
        {
            auto& c=i<visibleCreatures?creatures[i]:pal; auto& o=rc.lifeObjects[i];
            o[0]=c.p.x; o[1]=c.p.y; o[2]=c.p.z; o[3]=kLifeRadius[c.kind];
            o[4]=sinf(c.heading); o[5]=0; o[6]=cosf(c.heading); o[7]=(float)c.kind;
            o[8]=.5f+.5f*cosf(c.phase); o[9]=.5f+.5f*cosf(c.phase+2.094f); o[10]=.5f+.5f*cosf(c.phase+4.189f); o[11]=c.phase;
            o[12]=c.animation;o[13]=c.attention;o[14]=c.kind==LifeVrex?(float)palMode:0;o[15]=c.kind==LifeVrex?(float)palHand:c.startled?1.f:0.f;
        }
    }
};
