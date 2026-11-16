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
        Check(palConstants.vrexState!=(float)VrexPerched,"Vrex never automatically perches on a shoulder");
    }
    Check(palConstants.vrexState==(float)VrexBeamFollow,"Vrex follows a tracked beam");
    player.beams[0].active=false;life.SetPlayer(player);
    for(int frame=0;frame<300;++frame) life.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(palConstants.lifeObjects[0][15]==1,"Vrex switches to the available right beam");
    for(int frame=0;frame<25;++frame) {player.beams[1].origin.x+=.006f;life.SetPlayer(player);life.Fill(palConstants,palRoom,palSettings,1.f/60,false);}
    float beforeReverse=palConstants.lifeObjects[0][0];
    player.beams[1].origin.x-=.8f;life.SetPlayer(player);life.Fill(palConstants,palRoom,palSettings,1.f/120,false);
    Check(palConstants.lifeObjects[0][0]>beforeReverse,"Vrex retains momentum when a beam abruptly reverses");
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
    for(int frame=0;frame<300;++frame) companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    Check(((int)palConstants.vrexState&16)==0,"the host beam alone cannot open the companion menu");
    auto pointFree=[&](int row,bool trigger,float limit=20.f)
    {
        LifeVec position{palConstants.lifeObjects[0][0],palConstants.lifeObjects[0][1],palConstants.lifeObjects[0][2]};
        auto menu=MakeVrexMenuFrame(owner.head,position);
        LifeVec target=row?menu.centre+menu.up*(row==1?.054f:-.054f):position;
        LifeVec origin{.65f,-.25f,2.9f},direction=target-origin;
        owner.beams[1]={origin,direction*(1/direction.Length()),limit,true,trigger};
        companion.SetPlayer(owner);companion.Fill(palConstants,palRoom,palSettings,1.f/60,false);
    };
    for(int frame=0;frame<20;++frame) pointFree(0,false);
    Check(((int)palConstants.vrexState&16)!=0,"free-controller hover opens the Vrex interaction menu");
    pointFree(1,false);pointFree(1,true);pointFree(1,false);
    Check(palConstants.vrexChat==1 && (((int)palConstants.vrexState>>8)&3)==1,"Hello selects a greeting with a wave and wink");
    pointFree(2,false);pointFree(2,true);pointFree(0,true);pointFree(0,false);
    Check(palConstants.vrexChat==1,"moving off a pressed option cancels its reply");
    pointFree(2,false);pointFree(2,true);pointFree(2,false);
    Check(palConstants.vrexChat==7 && (((int)palConstants.vrexState>>8)&3)==2,"What are you doing selects a beam-following reply and shrug");
    for(int frame=0;frame<110;++frame) pointFree(1,false,.01f);
    Check(((int)palConstants.vrexState&16)==0,"foreground occluders prevent menu interaction and let it close");
    for(int frame=0;frame<20;++frame) pointFree(0,false);
    Check(((int)palConstants.vrexState&16)!=0,"menu reopens after moving clear of an occluder");
    owner.beams[1].active=false;companion.SetPlayer(owner);companion.Fill(palConstants,palRoom,palSettings,.02f,false);
    Check(((int)palConstants.vrexState&16)==0,"lost free-controller tracking closes the companion menu");
    palSettings.vrex=0;companion.Fill(palConstants,palRoom,palSettings,.02f,false);
    Check(palConstants.lifeCount==0 && ((int)palConstants.vrexState&16)==0,"Vrex off also disables his interaction menu");
    puts("PASS: FFT bands, silence, 18 room/curve collision soaks, species selection, toggles and directed relations");
}
