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
inline float LifeDot(LifeVec a,LifeVec b) {return a.x*b.x+a.y*b.y+a.z*b.z;}


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
    float palChatTimer=0;
    int palChat=0;
    float palGestureTimer=0;
    int palGesture=0;
    bool helperActive=false, helperPoint=false;
    int helperState=0;
    LifeVec helperTarget, helperAnchor;
    bool helperAnchored=false;
    bool helperRoam=false, roamingHome=false;
    float roamUntil=0;
    LifeVec roamOffset;
    float palPitch=0;
    uint32_t palAvatar=0, palShell=0;
    float PalScale() const { return palAvatar==0?kLifeRadius[LifeVrex]:.23f; }
    float PalRadius() const { return PalScale()*(palAvatar==2?1.45f:palAvatar==1?1.10f:1.f); }
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
        if(palActive && skip!=24 && (p-pal.p).Length()<radius+PalRadius()+.015f) return false;
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
    void SetVrexAppearance(uint32_t avatar,uint32_t shell)
    {
        avatar=std::min(2u,avatar);
        if(palAvatar!=avatar) palActive=false;
        palAvatar=avatar;
        palShell=shell & 0xffffffu;
    }
    void SetVrexWork(bool active,int state,bool hasPoint,LifeVec point,bool anchored=false,LifeVec anchor={},bool roam=false)
    {
        helperActive=active;helperState=state;helperPoint=active && hasPoint;helperTarget=point;
        helperAnchored=anchored;helperAnchor=anchor;
        helperRoam=active && hasPoint && roam;
        if(!helperRoam) roamUntil=0;
    }
    bool GetVrexPosition(LifeVec& position) const
    {
        position=pal.p;
        return palActive && player.valid;
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
            if(picked==count) {palChat=beam.trigger?3:0;palChatTimer=3;}
        }
    }
    void UpdateVrex(const Room& r,float dt,bool enabled,bool reset)
    {
        if(!enabled || !player.valid) {palActive=false;return;}
        LifeVec forward=player.forward;forward.y=0;float fl=forward.Length();forward=fl>.001f?forward*(1/fl):LifeVec{0,0,-1};
        LifeVec right{-forward.z,0,forward.x};
        LifeVec fallback=player.head+forward*.75f+right*.35f+LifeVec{0,-.12f,0};
        palChatTimer=std::max(0.f,palChatTimer-dt);
        palGestureTimer=std::max(0.f,palGestureTimer-dt);
        if(palGestureTimer<=0) palGesture=0;
        LifeVec target=fallback;
        if(helperAnchored)
            target=helperAnchor+LifeVec{.035f*sinf(clock*1.6f),.035f*sinf(clock*2.1f),0};
        if(helperActive)
        {
            palChatTimer=0;
            if(helperState==6 && helperPoint && helperAnchored)
            {
                target=helperAnchor;
                palGesture=3;palGestureTimer=.15f;
            }
            else if(helperPoint && !helperRoam && (helperState==1 || helperState==2))
            {
                if(helperAnchored) target.y+=std::clamp(helperTarget.y-helperAnchor.y,-.34f,.28f)*.65f;
                else target=helperTarget+LifeVec{-.20f,.10f,0};
                palGesture=3;palGestureTimer=.15f;
            }
            if(helperState==4) {palGesture=1;palGestureTimer=.15f;}
        }
        auto clearOfPointer=[&](LifeVec position)
        {
            if(!helperRoam) return true;
            LifeVec toPointer=helperTarget-player.head;
            float lengthSquared=LifeDot(toPointer,toPointer);
            if(lengthSquared<.001f) return true;
            float along=std::clamp(LifeDot(position-player.head,toPointer)/lengthSquared,0.f,1.f);
            return (position-(player.head+toPointer*along)).Length()>PalRadius()+.19f;
        };
        if(helperRoam)
        {
            if(clock>=roamUntil)
            {
                roamingHome=roamUntil>0 && !roamingHome && Rand()<.36f;
                roamUntil=clock+(roamingHome?2.5f+3.f*Rand():4.f+6.f*Rand());
                float side=Rand()<.5f?-1.f:1.f;
                roamOffset={side*(.65f+1.2f*Rand()),.30f+.55f*Rand(),.55f+1.35f*Rand()};
            }
            if(!roamingHome || !clearOfPointer(target))
            {
                float width=std::max(.05f,r.X-PalRadius()-.06f);
                LifeVec waypoint=helperTarget+roamOffset;
                waypoint.x=std::clamp(waypoint.x,-width,width);
                waypoint.y=std::clamp(waypoint.y,r.yF+PalRadius(),r.yC-PalRadius());
                if(!clearOfPointer(waypoint) || !Free(r,waypoint,PalRadius(),24))
                    waypoint=helperTarget+LifeVec{-roamOffset.x,roamOffset.y*.7f,roamOffset.z*.75f};
                if(clearOfPointer(waypoint) && Free(r,waypoint,PalRadius(),24)) target=waypoint;
            }
            if(!clearOfPointer(target)) target=pal.p;
        }
        if(!palActive || reset)
        {
            palActive=false;pal={};pal.kind=LifeVrex;pal.phase=.4f;
            if(!Free(r,target,PalRadius(),24)) target=fallback;
            if(!Free(r,target,PalRadius(),24)) return;
            pal.p=target;palActive=true;palChat=0;palChatTimer=3;palPitch=0;
        }
        if(!Free(r,target,PalRadius(),24)) target=pal.p;
        // A damped spring lets Vrex settle smoothly around a moving window.
        int steps=std::max(1,(int)ceilf(dt*120));float step=dt/steps;
        for(int tick=0;tick<steps;++tick)
        {
            LifeVec acceleration=(target-pal.p)*18.f-pal.v*4.5f;
            float force=acceleration.Length();if(force>10) acceleration=acceleration*(10/force);
            pal.v=pal.v+acceleration*step;float speed=pal.v.Length();if(speed>2.2f) pal.v=pal.v*(2.2f/speed);
            LifeVec next=pal.p+pal.v*step;
            // A moving cursor can enter the current clearance zone. Let Vrex
            // retreat from it, while preventing a normal flight across the beam.
            if(Free(r,next,PalRadius(),24) && (clearOfPointer(next) || !clearOfPointer(pal.p))) pal.p=next;
            else for(int axis=0;axis<3;++axis)
            {
                LifeVec slide=pal.p;
                if(axis==0) slide.x=next.x;if(axis==1) slide.y=next.y;if(axis==2) slide.z=next.z;
                if(Free(r,slide,PalRadius(),24) && (clearOfPointer(slide) || !clearOfPointer(pal.p))) pal.p=slide;
                else {if(axis==0) pal.v.x*=-.25f;if(axis==1) pal.v.y*=-.25f;if(axis==2) pal.v.z*=-.25f;}
            }
        }
        bool pointingAtUbuntu=helperActive && helperPoint;
        LifeVec look=(pointingAtUbuntu?helperTarget:player.head)-pal.p;float angle=atan2f(look.x,look.z);
        float speed=pal.v.Length();
        if(!pointingAtUbuntu && speed>.35f && palGesture==0) angle=atan2f(pal.v.x,pal.v.z);
        // A slow, eased pirouette between interactions; returning to 2*pi is the
        // same orientation, so the end of the cycle never snaps.
        float turn=fmodf(clock,19.f)-15.f;
        if(!pointingAtUbuntu && turn>0 && turn<2.4f && speed<.15f && palGesture==0)
        {float q=turn/2.4f;angle+=6.2831853f*q*q*(3-2*q);}
        pal.heading+=std::remainder(angle-pal.heading,6.2831853f)*(1-expf(-dt*5));
        LifeVec facing{sinf(pal.heading),0,cosf(pal.heading)};
        LifeVec force=(target-pal.p)*18.f-pal.v*4.5f;
        float pitch=std::clamp(.20f*(pal.v.x*facing.x+pal.v.z*facing.z)+
            .10f*(force.x*facing.x+force.z*facing.z),-.42f,.48f);
        palPitch+=(pitch-palPitch)*(1-expf(-dt*6));
        pal.animation+=dt;
        if(!helperActive && palChatTimer<=0 && (int)clock%13==0) {palChat=smooth.bass>.15f?2:5;palChatTimer=3;}
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
        if(reset) {palActive=false;palGestureTimer=0;palGesture=0;}
        if((s.flags&4) && (reset || oldMask!=s.species || oldWanted!=s.count || count==0)) { Populate(r,s); oldMask=s.species; oldWanted=s.count; }
        if(!(s.flags&4)) count=0;
        ReactToBeams(r,std::clamp(dt,0.f,.1f),s.beams!=0);
        UpdateVrex(r,std::clamp(dt,0.f,.1f),s.vrex!=0,reset);
        if(s.flags&4)
        {
            accumulator+=std::clamp(dt,0.f,.1f);
            while(accumulator>=1.f/120) { Step(r,1.f/120,sound.bass); accumulator-=1.f/120; }
        }
        rc.lifeTime=clock; rc.audioBass=sound.bass; rc.audioMid=sound.mid; rc.audioHigh=sound.high;
        rc.lifeBeat=sound.beat; rc.audioLevel=sound.level; rc.lifeGain=s.gain*.01f; rc.particleDensity=(float)s.density;
        rc.lifePreset=s.preset; rc.lifeSpecies=s.species; rc.lifeFlags=s.flags|(s.vrex?128:0);
        rc.visualReactivity=s.reactivity*.01f; rc.visualLayers=s.layers<0?1u<<s.preset:(uint32_t)s.layers;
        int gesture=palGesture?palGesture:(pal.v.Length()>.35f?3:0);
        rc.vrexChat=helperActive?0:(palChatTimer>0?(float)(palChat+1):0);
        // Packed into the existing float to retain the shared constant ABI:
        // Reserved[0:7], gesture[8:9], speaking[10].
        rc.vrexState=(float)(256*gesture+
            (helperActive && helperState==4 ? 1024 : 0));
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
            o[12]=c.animation;o[13]=c.attention;o[14]=c.kind==LifeVrex?palPitch:0;o[15]=c.kind==LifeVrex?-1.f:c.startled?1.f:0.f;
            if(c.kind==LifeVrex)
            {
                o[3]=PalScale();
                o[7]=(float)(LifeVrex+palAvatar);
                uint32_t shell=palShell?palShell:0x06947du;
                o[8]=((shell>>16)&255u)/255.f;
                o[9]=((shell>>8)&255u)/255.f;
                o[10]=(shell&255u)/255.f;
            }
        }
    }
};
