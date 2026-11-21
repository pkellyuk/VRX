#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "room_life.h"
#include "desktop_control.h"
#include <cstdlib>

static void Check(bool yes,const char* message) { if(!yes) { printf("FAIL: %s\n",message); exit(1); } }
int main(int argc,char**)
{
    std::array<float,1024> samples{};
    auto silent=RoomAudio::Analyse(samples,48000);
    Check(silent.level==0 && silent.bass==0 && silent.mid==0 && silent.high==0,"silence has no energy");
    for(int band=0;band<3;++band)
    {
        float hz=band==0?94.f:band==1?1500.f:9000.f;
        for(int i=0;i<1024;++i) samples[i]=.2f*sinf(6.2831853f*hz*i/48000);
        auto f=RoomAudio::Analyse(samples,48000);
        Check(f.level>.4,"tone RMS");
        Check(band==0?f.bass>f.mid*5 && f.bass>f.high*5:band==1?f.mid>f.bass*5 && f.mid>f.high*5:f.high>f.bass*5 && f.high>f.mid*5,"FFT assigns bass/mid/treble correctly");
    }
    if(argc>1)
    {
    RoomAudio capture;
    capture.Enable(true);
    for(int wait=0;wait<200 && capture.Status()!=2;++wait) Sleep(10);
    Check(capture.Status()==2,"WASAPI opens the current Windows playback output");
    capture.Enable(false);
    Check(capture.Read().level==0,"disabling capture immediately clears its public signal");
    }
    LifeCreature left{{-.23f,0,0},{.5f,0,0},LifeRobot,0},right{{.23f,0,0},{-.5f,0,0},LifeRobot,0};
    RoomLife::ResolveContact(left,right);
    Check(left.v.x<0 && right.v.x>0 && fabsf(left.v.x+right.v.x)<.00001f,"creature contacts exchange momentum and bounce apart");
    RoomLife life;
    for(int env=0;env<9;++env) for(int curve=0;curve<2;++curve)
    {
        RoomInputs in; in.W=5.7f; in.H=3.2f; in.eye[2]=3; in.floorY=-1.5f; in.env=env;
        if(curve) Check(BuildCylinder(in.W,in.H,3,1,in.cyl),"test cylinder");
        Room r; Check(BuildRoom(in,r),"build test room");
        Check(r.valid,"valid test room");
        RoomLifeSettings settings; settings.flags=4; settings.count=24;
        RoomConstants rc;
        life.Fill(rc,r,settings,0,true);
        Check(rc.lifeCount>=8 && rc.lifeCount<=24,"room population");
        for(int frame=0;frame<1800;++frame)
        {
            RoomConstants previous=rc;
            life.Fill(rc,r,settings,1.f/60,false);
            for(unsigned i=0;i<rc.lifeCount;++i)
            {
                auto& o=rc.lifeObjects[i];
                LifeVec movement{o[0]-previous.lifeObjects[i][0],o[1]-previous.lifeObjects[i][1],o[2]-previous.lifeObjects[i][2]};
                Check(movement.Length()<.04f,"creatures move continuously, including theatre steps");
                Check(RoomLife::Clear(r,{o[0],o[1],o[2]},o[3]),"no wall/floor/ceiling/furniture penetration over 30 seconds");
                for(unsigned j=0;j<i;++j)
                {
                    auto& b=rc.lifeObjects[j]; float dx=o[0]-b[0],dy=o[1]-b[1],dz=o[2]-b[2];
                    Check(sqrtf(dx*dx+dy*dy+dz*dz)+.0001f>=o[3]+b[3],"no creature penetration over 30 seconds");
                }
            }
        }
        settings.species=1<<LifeRobot; life.Fill(rc,r,settings,.1f,false);
        for(unsigned i=0;i<rc.lifeCount;++i) Check(rc.lifeObjects[i][7]==(float)LifeRobot,"species selector");
        settings.flags=0; life.Fill(rc,r,settings,.1f,false);
        Check(rc.lifeCount==0 && rc.lifeFlags==0 && rc.audioLevel==0,"off disables creatures and audio");
    }
    RoomInputs palInputs;palInputs.W=5.7f;palInputs.H=3.2f;palInputs.eye[2]=3;
    Room palRoom;Check(BuildRoom(palInputs,palRoom),"Vrex test room");
    RoomLifeSettings palSettings;palSettings.vrex=1;
    LifePlayer player;player.valid=true;player.head={0,0,3};player.forward={0,0,-1};
    life.SetPlayer(player);RoomConstants palConstants;life.Fill(palConstants,palRoom,palSettings,0,true);
    Check(RoomLookOn(palConstants),"Vrex alone selects the room effects shader");
    player.beams[0]={{-.25f,-.25f,2.9f},{0,0,-1},3,true,false};
    player.beams[1]={{.65f,-.25f,2.9f},{0,0,-1},3,true,false};
    life.SetPlayer(player);
    for(int frame=0;frame<1600;++frame)
    {
        auto previous=LifeVec{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
        life.Fill(palConstants,palRoom,palSettings,1.f/60,false);
        Check(palConstants.lifeCount==1 && palConstants.lifeObjects[0][7]==(float)LifeVrex,"Vrex is independent of creature population");
        LifeVec position{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
        Check(RoomLife::Clear(palRoom,position,kLifeRadius[LifeVrex]),"Vrex respects room furniture and boundaries");
        Check((position-player.head).Length()>=.30f,"Vrex does not fly through the viewer head");
        Check((position-previous).Length()<.04f,"Vrex moves continuously");
    }
    const LifeVec chatAnchor{-1.0f,.15f,1.75f};
    life.SetVrexWork(true,0,false,{},true,chatAnchor);
    for(int frame=0;frame<360;++frame)
    {
        player.beams[1].origin.x=.65f+sinf(frame*.08f);life.SetPlayer(player);
        life.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    }
    LifeVec quietPosition{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
    Check((quietPosition-chatAnchor).Length()<.15f && palConstants.lifeObjects[0][15]==-1,
        "Vrex hovers by the chat anchor while the controller beam moves");
    life.SetVrexWork(false,0,false,{});
    player.beams[1].active=false;
    LifeVec palPosition{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
    LifeVec toPal=palPosition-player.head;player.beams[0]={player.head,toPal*(1/toPal.Length()),20,true,true};
    life.SetPlayer(player);life.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(palConstants.lifeObjects[0][13]>0 && palConstants.vrexChat==4,"Vrex responds to a tracked trigger beam with a chat bubble");
    player.beams[0].limit=.01f;life.SetPlayer(player);float alert=palConstants.lifeObjects[0][13];
    life.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(palConstants.lifeObjects[0][13]<alert,"a foreground UI panel blocks beam reactions");
    Check(kLifeRelations[LifeFish][LifeShark]==Flee && kLifeRelations[LifeShark][LifeFish]==Chase,"directed interaction table");
    RoomLife companion;LifePlayer owner;owner.valid=true;owner.head={0,0,3};owner.forward={0,0,-1};
    owner.beams[0]={{-.25f,-.25f,2.9f},{0,0,-1},3,true,false};
    companion.SetPlayer(owner);companion.Fill(palConstants,palRoom,palSettings,0,true);
    companion.SetVrexWork(true,4,false,{});
    companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(((int)palConstants.vrexState&1024)!=0 && palConstants.vrexChat==0,"spoken replies animate Vrex without a preset bubble");
    companion.SetVrexWork(true,0,false,{});
    for(int frame=0;frame<60;++frame)
    {
        LifeVec position{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
        LifeVec origin{.65f,-.25f,2.9f},direction=position-origin;
        owner.beams[1]={origin,direction*(1/direction.Length()),20.f,true,frame%20>=10};
        companion.SetPlayer(owner);companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
        Check(((int)palConstants.vrexState&0xf0)==0 && palConstants.vrexChat==0,
            "Aiming and clicking at Vrex never opens the old prompts");
    }
    LifeVec companionPosition;
    Check(companion.GetVrexPosition(companionPosition),"Vrex reports his position for spatial audio");
    owner.beams[1].active=false;companion.SetPlayer(owner);
    companion.SetVrexWork(true,2,true,{.6f,0,2});
    for(int frame=0;frame<300;++frame)companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    LifeVec working{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
    Check(palConstants.lifeCount==1 && palConstants.vrexChat==0,"Ubuntu helper reuses one Vrex and suppresses canned replies");
    Check((working-LifeVec{.4f,.1f,2}).Length()<.1f && (((int)palConstants.vrexState>>8)&3)==3,"working Vrex follows and points at the Ubuntu interaction target");
    Check(palConstants.lifeObjects[0][4]>.95f && fabsf(palConstants.lifeObjects[0][6])<.1f,"working Vrex faces the Ubuntu target rather than pointing toward the viewer");
    const LifeVec pointAnchor{.75f,.16f,2};
    companion.SetVrexWork(true,6,true,{.93f,.1f,2},true,pointAnchor);
    for(int frame=0;frame<240;++frame)companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(companion.GetVrexPosition(companionPosition) && (companionPosition-pointAnchor).Length()<.1f &&
        (((int)palConstants.vrexState>>8)&3)==3,"the explicit point gesture flies to its screen anchor");
    companion.SetVrexWork(true,0,false,{},true,working);
    for(int frame=0;frame<240;++frame)companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(companion.GetVrexPosition(companionPosition) && (companionPosition-working).Length()<.13f,
        "the companion returns to his normal anchor after pointing");
    RoomLife roaming;
    owner.beams[0].active=false; owner.beams[1].active=false;
    roaming.SetPlayer(owner);
    const LifeVec focus{0,0,0},home{-1.4f,.1f,1.8f},beam=focus-owner.head;
    roaming.SetVrexWork(true,0,true,focus,true,home,true);
    bool visitedScreen=false, returnedHome=false;
    for(int frame=0;frame<4200;++frame)
    {
        roaming.Fill(palConstants,palRoom,palSettings,1.f/60,frame==0);
        Check(palConstants.lifeCount==1,"roaming Vrex remains present");
        LifeVec position{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
        float along=std::clamp(LifeDot(position-owner.head,beam)/LifeDot(beam,beam),0.f,1.f);
        Check((position-(owner.head+beam*along)).Length()>=kLifeRadius[LifeVrex]+.18f,"roaming Vrex avoids the cursor's sightline");
        if(position.z<1.2f && (position-home).Length()>.9f) visitedScreen=true;
        if(visitedScreen && (position-home).Length()<.18f) returnedHome=true;
    }
    Check(visitedScreen && returnedHome,"helpful Vrex explores near the screen and returns to Ubuntu");
    companion.SetVrexWork(false,0,false,{});
    palSettings.vrex=0;companion.Fill(palConstants,palRoom,palSettings,.02f,false);
    Check(palConstants.lifeCount==0 && ((int)palConstants.vrexState&16)==0,"Vrex off also disables his interaction menu");
    puts("PASS: FFT bands, silence, 18 room/curve collision soaks, species selection, toggles and directed relations");
}
