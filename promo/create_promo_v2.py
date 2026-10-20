"""Make the motion-led VRX3D promo using the site's own product images.

The neon room and transparent headset are graphic illustrations. Product views
are the unaltered screenshots published on vrx3d.uk.
"""

from pathlib import Path
import math
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
W, H, FPS, DURATION = 1280, 720, 30, 29.0
CYAN = (232, 229, 74)  # BGR
PINK = (194, 77, 245)
VIOLET = (234, 115, 126)
WHITE = (247, 248, 250)
FONT = r"C:\Windows\Fonts\bahnschrift.ttf"
BOLD = r"C:\Windows\Fonts\segoeuib.ttf"


def ease(v):
    v = max(0, min(1, v))
    return v*v*(3-2*v)


def clamp(v, lo=0, hi=1):
    return max(lo, min(hi, v))


def cover(path, w=W, h=H):
    img = cv2.imread(str(ROOT / "assets" / path), cv2.IMREAD_COLOR)
    if img is None:
        raise FileNotFoundError(path)
    ih, iw = img.shape[:2]
    s = max(w/iw, h/ih)
    nw, nh = math.ceil(iw*s), math.ceil(ih*s)
    img = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_CUBIC)
    x, y = (nw-w)//2, (nh-h)//2
    return img[y:y+h, x:x+w].copy()


IMAGES = {n:cover(n) for n in ["helldivers.jpg", "system-shock.jpg", "cyberpunk.jpg", "skyrim.jpg", "elite.jpg", "cyberpunk-depth.png"]}
UI = cv2.imread(str(ROOT / "assets" / "easy-mode.png"))
LOGO = cv2.imread(str(ROOT / "assets" / "logo.png"), cv2.IMREAD_UNCHANGED)
YY = np.linspace(0, 1, H, dtype=np.float32)[:,None,None]
BACKGROUND = np.repeat(np.uint8((np.array([14, 10, 9])[None,None,:]*(1-YY)+np.array([39, 21, 16])[None,None,:]*YY)), W, axis=1)


def text_layer(lines, x, y, sizes, colors=None, anchor="left", gap=14):
    """Rasterize anti-aliased captions once, then alpha blend during animation."""
    im = Image.new("RGBA", (W,H), (0,0,0,0))
    d = ImageDraw.Draw(im)
    cy = y
    for i, line in enumerate(lines):
        size = sizes[i] if isinstance(sizes, list) else sizes
        col = colors[i] if colors else (250,250,250,255)
        f = ImageFont.truetype(BOLD if size >= 32 else FONT, size)
        d.text((x,cy), line, font=f, fill=col, anchor="mt" if anchor=="center" else None,
               stroke_width=0)
        cy += size+gap
    return cv2.cvtColor(np.asarray(im), cv2.COLOR_RGBA2BGRA)


CAPTIONS = {
    "intro": text_layer(["FLAT GAMES", "MEET REAL DEPTH"], 86, 96, [61,61], [(255,255,255,255),(101,238,241,255)]),
    "proof": text_layer(["FROM YOUR SCREEN", "TO YOUR HEADSET"], 72, 82, [52,52]),
    "depth": text_layer(["EVERY FRAME", "GETS DEPTH"], 72, 69, [54,54]),
    "easy": text_layer(["START GAME", "VRX3D ATTACHES", "PLAY"], 80, 92, [55,55,55], [(245,245,245,255),(101,238,241,255),(245,245,245,255)]),
    "room": text_layer(["A ROOM THAT", "REACTS TO THE GAME"], 76, 75, [53,53]),
    "end": text_layer(["VRX3D", "PLAY FLAT. EXPERIENCE DEPTH.", "FREE & OPEN SOURCE", "vrx3d.uk"], W//2, 205,
                      [102,35,25,42], [(255,255,255,255),(101,238,241,255),(220,211,229,255),(255,255,255,255)], anchor="center", gap=30),
}


def put_rgba(frame, layer, opacity=1, offset=(0,0)):
    if opacity <= 0:
        return
    ox, oy = offset
    x0,y0 = max(0,ox), max(0,oy)
    x1,y1 = min(W,ox+layer.shape[1]), min(H,oy+layer.shape[0])
    if x0>=x1 or y0>=y1: return
    src = layer[y0-oy:y1-oy,x0-ox:x1-ox]
    alpha = src[:,:,3:4].astype(np.float32) * (opacity/255)
    if alpha.max() == 0: return
    dst = frame[y0:y1,x0:x1]
    dst[:] = np.uint8(dst*(1-alpha) + src[:,:,:3]*alpha)


def line(frame, glow, p, q, color, thick=1):
    cv2.line(frame,p,q,color,thick,cv2.LINE_AA)
    cv2.line(glow,p,q,color,max(2,thick*3),cv2.LINE_AA)


def project(x,y,z,camz=0,cx=0,cy=0, f=685):
    dz=z-camz
    if dz<0.16: return None
    return (int(W/2+f*(x-cx)/dz), int(H*.53-f*(y-cy)/dz))


def room(frame, t, camz=0, intensity=1):
    glow = np.zeros_like(frame)
    drift = 0.18*math.sin(t*.7)
    # A real perspective floor, walls, and a slightly curved cinema screen.
    zs = np.linspace(camz+1.4, camz+24, 18)
    for x in np.arange(-12,12.1,1.4):
        a=project(x,-1.35,camz+1.4,camz,drift)
        b=project(x,-1.35,camz+24,camz,drift)
        if a and b: line(frame,glow,a,b,(122,112,51),1)
    for z in zs:
        a,b=project(-12,-1.35,z,camz,drift),project(12,-1.35,z,camz,drift)
        if a and b: line(frame,glow,a,b,(125,110,55),1)
    for wallx,c in [(-5,CYAN),(5,PINK)]:
        for y in np.arange(-1.35,3.6,1.0):
            a,b=project(wallx,y,camz+2.0,camz,drift),project(wallx,y,camz+24,camz,drift)
            if a and b: line(frame,glow,a,b,tuple(int(v*.65) for v in c),1)
        for z in np.linspace(camz+3,camz+23,7):
            a,b=project(wallx,-1.35,z,camz,drift),project(wallx,3.4,z,camz,drift)
            if a and b: line(frame,glow,a,b,tuple(int(v*.7) for v in c),1)
    # Curved screen outline in the distance.
    last_top=last_bot=None
    for x in np.linspace(-3.7,3.7,26):
        z=18+0.45*(x/3.7)**2
        top,bot=project(x,2.45,z,camz,drift),project(x,-.72,z,camz,drift)
        if top and bot:
            if last_top: line(frame,glow,last_top,top,CYAN if x<0 else PINK,2)
            if last_bot: line(frame,glow,last_bot,bot,CYAN if x<0 else PINK,2)
            last_top,last_bot=top,bot
    if last_top:
        for x in [-3.7,3.7]:
            z=18.45
            a,b=project(x,-.72,z,camz,drift),project(x,2.45,z,camz,drift)
            if a and b: line(frame,glow,a,b,CYAN if x<0 else PINK,2)
    # Deterministic moving particles.
    for k in range(70):
        z=2+(k*7.73+t*3.5)%24
        x=math.sin(k*17.1)*7
        y=math.cos(k*10.9)*2+1.0
        p=project(x,y,z,camz,drift)
        if p and 0<=p[0]<W and 0<=p[1]<H:
            r=1 if z>8 else 2
            cv2.circle(frame,p,r,(165,130,190) if k%3 else CYAN,-1,cv2.LINE_AA)
    soft=cv2.GaussianBlur(glow,(0,0),8)
    cv2.addWeighted(frame,1,soft,.24*intensity,0,frame)


def headset(frame,t,zoom):
    """Transparent wireframe headset with an optical lens visible behind the visor."""
    glow=np.zeros_like(frame)
    camz=zoom
    camx=-.68*ease((t-3.6)/1.8)
    ang=.22*(1-ease((t-2)/3.4))
    def p(x,y,z=6):
        xx=x*math.cos(ang)+z*math.sin(ang)-6*math.sin(ang)
        zz=6+(z-6)*math.cos(ang)-x*math.sin(ang)
        return project(xx,y,zz,camz,camx,0,810)
    # Outline is curved in depth and subdivided, so camera travel changes its shape.
    def shell_y(x,y):
        edge=(abs(x)/1.75)**4
        if y<-.5: return y+.31*math.exp(-(x/.35)**2)+.08*edge
        if y>.5: return y-.12*edge
        return y
    def curve(y, extra=0):
        out=[]
        for x in np.linspace(-1.75,1.75,32):
            out.append(p(x,shell_y(x,y),6+.19*(x/1.75)**2+extra))
        return out
    for row,c in [(-.68,CYAN),(.68,PINK)]:
        pts=curve(row)
        for a,b in zip(pts,pts[1:]):
            if a and b: line(frame,glow,a,b,c,3)
    for x in np.linspace(-1.65,1.65,11):
        a,b=p(x,shell_y(x,-.62),6+.19*(x/1.75)**2),p(x,shell_y(x,.62),6+.19*(x/1.75)**2)
        if a and b: line(frame,glow,a,b,(110,91,85),1)
    for y in [-.38,0,.38]:
        pts=curve(y)
        for a,b in zip(pts,pts[1:]):
            if a and b: line(frame,glow,a,b,(122,103,89),1)
    for x in [-1.75,1.75]:
        a,b=p(x,shell_y(x,-.68),6.19),p(x,shell_y(x,.68),6.19)
        if a and b: line(frame,glow,a,b,CYAN if x<0 else PINK,3)
    for s in [-1,1]:
        # Head strap recedes to the back; optics sit in the transparent chassis.
        pa,pb,pc=p(s*1.72,.30,6.2),p(s*2.25,.25,7.0),p(s*1.78,.18,8.1)
        if pa and pb: line(frame,glow,pa,pb,CYAN if s<0 else PINK,2)
        if pb and pc: line(frame,glow,pb,pc,CYAN if s<0 else PINK,2)
        for radius,c in [(.52,(145,221,237)),(.42,CYAN if s<0 else PINK),(.29,(124,98,153))]:
            prev=None
            for a in np.linspace(0,2*math.pi,49):
                pt=p(s*.72+radius*math.cos(a),radius*.81*math.sin(a),5.91)
                if pt and prev: line(frame,glow,prev,pt,c,2)
                prev=pt
        c=p(s*.72,0,5.91)
        if c: cv2.circle(frame,c,max(3,int(8/(6-camz))),CYAN if s<0 else PINK,-1,cv2.LINE_AA)
    soft=cv2.GaussianBlur(glow,(0,0),10)
    cv2.addWeighted(frame,1,soft,.52,0,frame)


def image_motion(src,t,scale=1.0):
    z=scale*(1.0+.055*ease(t))
    cw,ch=int(W/z),int(H/z)
    x=int((W-cw)*(.33+.28*t))
    y=int((H-ch)*(.48+.08*math.sin(t*2)))
    return cv2.resize(src[y:y+ch,x:x+cw],(W,H),interpolation=cv2.INTER_LINEAR)


def tint(frame,bgr,alpha):
    shade=np.empty_like(frame);shade[:]=bgr
    cv2.addWeighted(frame,1-alpha,shade,alpha,0,frame)


def rounded_panel(frame,img,box,radius=18):
    x,y,w,h=box
    target=cv2.resize(img,(w,h),interpolation=cv2.INTER_AREA)
    mask=np.zeros((h,w),np.uint8)
    cv2.rectangle(mask,(radius,0),(w-radius-1,h-1),255,-1)
    cv2.rectangle(mask,(0,radius),(w-1,h-radius-1),255,-1)
    for xx in [radius,w-radius-1]:
        for yy in [radius,h-radius-1]:cv2.circle(mask,(xx,yy),radius,255,-1)
    roi=frame[y:y+h,x:x+w]
    roi[mask>0]=target[mask>0]
    cv2.rectangle(frame,(x,y),(x+w,y+h),CYAN,2,cv2.LINE_AA)


def frame_at(t):
    f=BACKGROUND.copy()
    if t<6.4:
        room(f,t,camz=.15*t)
        if t<2.0:
            put_rgba(f,CAPTIONS["intro"],ease(t/.7)*(1-ease((t-1.55)/.4)),offset=(0,int(24*(1-ease(t/.7)))))
        if t>=1.55:
            p=ease((t-1.55)/4.75)
            headset(f,t,zoom=4.86*p)
            if t<4.6:
                label=text_layer(["STEP INSIDE"],W//2,612,35,[(230,239,246,255)],anchor="center")
                put_rgba(f,label,ease((t-1.7)/.6)*(1-ease((t-4.0)/.6)))
        if t>5.1:
            # The headset's left optical lens expands to reveal the actual product view.
            target=image_motion(IMAGES["helldivers.jpg"],.0)
            rad=int(50+1150*ease((t-5.1)/1.25))
            cx,cy=W//2,H//2
            mask=np.zeros((H,W),np.uint8)
            cv2.circle(mask,(cx,cy),rad,255,-1,cv2.LINE_AA)
            f[mask>0]=target[mask>0]
            cv2.circle(f,(cx,cy),rad,CYAN,max(2,int(14*(1-ease((t-5.1)/1.25)))),cv2.LINE_AA)
        return f
    if t<11.15:
        p=t-6.4
        shots=[("helldivers.jpg",0.0),("cyberpunk.jpg",1.5),("system-shock.jpg",3.0)]
        idx=min(2,int(p/1.5))
        src=IMAGES[shots[idx][0]]
        f=image_motion(src,(p%1.5)/1.5)
        # Fast diagonal screen-edge sweeps make the three real product views distinct.
        tint(f,(16,12,12),.19)
        cv2.line(f,(0,20),(W,20),CYAN,4,cv2.LINE_AA)
        cv2.line(f,(0,H-24),(W,H-24),PINK,3,cv2.LINE_AA)
        cv2.rectangle(f,(47,50),(665,193),(12,15,23),-1)
        put_rgba(f,CAPTIONS["proof"],ease((p%1.5)/.25))
        cv2.putText(f,["01 / GAMES","02 / STEREO SCREEN","03 / VR ROOM"][idx],(70,640),cv2.FONT_HERSHEY_SIMPLEX,.73,WHITE,2,cv2.LINE_AA)
        return f
    if t<15.8:
        p=t-11.15
        photo=image_motion(IMAGES["cyberpunk.jpg"],.2)
        dep=image_motion(IMAGES["cyberpunk-depth.png"],.2)
        f=photo.copy()
        split=int(W*(1-ease((p-.75)/1.7)))
        if split<W: f[:,split:]=dep[:,split:]
        tint(f,(12,12,18),.25)
        if 0<split<W: cv2.line(f,(split,0),(split,H),CYAN,6,cv2.LINE_AA)
        cv2.rectangle(f,(47,43),(660,191),(12,15,23),-1)
        put_rgba(f,CAPTIONS["depth"],ease(p/.35))
        cv2.putText(f,"GAME",(55,656),cv2.FONT_HERSHEY_SIMPLEX,.85,WHITE,2,cv2.LINE_AA)
        cv2.putText(f,"DEPTH MAP",(1025,656),cv2.FONT_HERSHEY_SIMPLEX,.8,CYAN,2,cv2.LINE_AA)
        return f
    if t<20.7:
        p=t-15.8
        room(f,t,camz=2.2)
        cv2.rectangle(f,(55,54),(580,332),(12,15,23),-1)
        put_rgba(f,CAPTIONS["easy"],ease(p/.35))
        scale=.72+.22*ease((p-.3)/1.4)
        uw,uh=int(UI.shape[1]*scale),int(UI.shape[0]*scale)
        panel=cv2.resize(UI,(uw,uh),interpolation=cv2.INTER_CUBIC)
        x,y=W-uw-45,145
        cv2.rectangle(f,(x-13,y-13),(x+uw+13,y+uh+13),(15,22,34),-1)
        f[y:y+uh,x:x+uw]=panel
        cv2.rectangle(f,(x-13,y-13),(x+uw+13,y+uh+13),CYAN,3,cv2.LINE_AA)
        cv2.line(f,(85,532),(514,532),(69,88,100),4)
        q=ease((p-.5)/3.2)
        cv2.line(f,(85,532),(85+int(429*q),532),CYAN,6,cv2.LINE_AA)
        for xx,lab in [(85,"GAME"),(300,"AUTO ATTACH"),(514,"VR")]:
            cv2.circle(f,(xx,532),7,CYAN,-1,cv2.LINE_AA)
            cv2.putText(f,lab,(xx-7,565),cv2.FONT_HERSHEY_SIMPLEX,.56,WHITE,1,cv2.LINE_AA)
        return f
    if t<25.35:
        p=t-20.7
        room(f,t,camz=1.7+1.0*ease(p/4.5))
        shot=IMAGES["elite.jpg"] if p<2.3 else IMAGES["skyrim.jpg"]
        # A screen travels forward through the matching wireframe room.
        sw=int(690+170*ease(p/4.6)); sh=int(sw*.69)
        x=(W-sw)//2;y=(H-sh)//2+28
        rounded_panel(f,shot,(x,y,sw,sh),20)
        cv2.rectangle(f,(29,30),(925,152),(12,15,23),-1)
        put_rgba(f,CAPTIONS["room"],ease(p/.4))
        tags=["CURVED SCREEN","AMBIENT LIGHT","REFLECTIONS"]
        tag=tags[min(2,int(p/1.55))]
        cv2.rectangle(f,(W-330,H-102),(W-28,H-41),(12,15,23),-1)
        cv2.putText(f,tag,(W-316,H-63),cv2.FONT_HERSHEY_SIMPLEX,.68,CYAN,2,cv2.LINE_AA)
        return f
    p=t-25.35
    room(f,t,camz=3.2+2.5*ease(p/3.65))
    tint(f,(13,10,20),.32)
    if LOGO is not None:
        sz=int(130+25*ease(p/2.5))
        lg=cv2.resize(LOGO,(sz,sz),interpolation=cv2.INTER_AREA)
        x,y=(W-sz)//2,54
        if lg.shape[2]==4:
            a=lg[:,:,3:4].astype(np.float32)/255
            f[y:y+sz,x:x+sz]=np.uint8(f[y:y+sz,x:x+sz]*(1-a)+lg[:,:,:3]*a)
        else: f[y:y+sz,x:x+sz]=lg
    cv2.rectangle(f,(226,192),(1054,590),(11,14,25),-1)
    cv2.rectangle(f,(226,192),(1054,590),CYAN,2)
    put_rgba(f,CAPTIONS["end"],ease(p/.7))
    if p>3.0:
        f=np.uint8(f*(1-.13*ease((p-3.0)/.65)))
    return f


def main():
    dest=ROOT/"promo_v2_silent.mp4"
    writer=cv2.VideoWriter(str(dest),cv2.VideoWriter_fourcc(*"mp4v"),FPS,(W,H))
    if not writer.isOpened():raise RuntimeError("Video encoder unavailable")
    for n in range(int(DURATION*FPS)):
        writer.write(frame_at(n/FPS))
        if n%150==0:print(f"{n/FPS:.1f}s / {DURATION:.1f}s",flush=True)
    writer.release()
    print(dest)

if __name__=="__main__":main()
