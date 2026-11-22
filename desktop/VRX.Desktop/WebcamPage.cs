using CefSharp;
using CefSharp.Enums;
using CefSharp.Handler;

namespace VRX.Desktop;

// Served by Chromium's resource handler. There is no local web server and no
// network request for the page or video. Only this panel can auto-grant video.
internal sealed class WebcamPage : ISchemeHandlerFactory
{
    internal const string Host = "vrx-webcam.invalid";
    internal const string Url = "https://" + Host + "/";

    public IResourceHandler? Create(IBrowser browser, IFrame frame, string schemeName, IRequest request) =>
        request.Url == Url ? ResourceHandler.FromString(page, mimeType: "text/html") : null;

    internal sealed class LocalNavigation : RequestHandler
    {
        protected override bool OnBeforeBrowse(IWebBrowser control, IBrowser browser, IFrame frame,
            IRequest request, bool userGesture, bool isRedirect) => request.Url != Url;
    }

    internal sealed class CameraPermissions : PermissionHandler
    {
        protected override bool OnRequestMediaAccessPermission(IWebBrowser control, IBrowser browser, IFrame frame,
            string requestingOrigin, MediaAccessPermissionType requestedPermissions, IMediaAccessCallback callback)
        {
            using (callback)
            {
                if (frame.IsMain && frame.Url == Url && requestingOrigin.TrimEnd('/') == Url.TrimEnd('/') &&
                    requestedPermissions == MediaAccessPermissionType.VideoCapture)
                    callback.Continue(MediaAccessPermissionType.VideoCapture);
                else callback.Cancel();
            }
            return true;
        }
    }

    private readonly string page = $$$$"""
        <!doctype html><html lang="{{{{System.Globalization.CultureInfo.CurrentUICulture.Name}}}}"><head><meta charset="utf-8">
        <meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; media-src blob:">
        <meta name="viewport" content="width=device-width,initial-scale=1">
        <title>{{{{System.Net.WebUtility.HtmlEncode(Loc.Get("FloatingWebcam"))}}}}</title><style>
          *{box-sizing:border-box}body{margin:0;background:#101824;color:#e9edf4;font:19px 'Segoe UI',sans-serif;height:100vh;display:grid;grid-template-rows:auto 1fr}
          header{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#202f41}
          select,button{min-height:39px;border:1px solid #5d7485;border-radius:8px;background:#152232;color:#fff;font:inherit;padding:5px 10px}
          select{max-width:45vw}button{cursor:pointer}button:disabled{opacity:.55}#status{margin-left:auto;font-size:16px}
          main{min-height:0;display:grid;place-items:center;background:#090d15;overflow:hidden}
          video{width:100%;height:100%;object-fit:contain;display:block}
        </style></head><body>
        <header><label for="devices">{{{{System.Net.WebUtility.HtmlEncode(Loc.Get("BrowserPermissionCamera"))}}}}</label><select id="devices"><option value="">{{{{System.Net.WebUtility.HtmlEncode(Loc.Get("WebcamDefaultCamera"))}}}}</option></select>
        <button id="start">{{{{System.Net.WebUtility.HtmlEncode(Loc.Get("WebcamStartCamera"))}}}}</button><button id="stop" disabled>{{{{System.Net.WebUtility.HtmlEncode(Loc.Get("VrexStop"))}}}}</button>
        <span id="status" role="status">{{{{System.Net.WebUtility.HtmlEncode(Loc.Get("WebcamCameraOff"))}}}}</span></header><main><video id="video" autoplay muted playsinline></video></main>
        <script>
          const devices=document.getElementById('devices'), video=document.getElementById('video'),
            start=document.getElementById('start'), stop=document.getElementById('stop'), status=document.getElementById('status');
          let stream=null, generation=0;
          function release(){ if(stream) stream.getTracks().forEach(track=>track.stop()); stream=null; video.srcObject=null; }
          function closeCamera(){ ++generation; release(); start.disabled=false; stop.disabled=true; status.textContent={{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamCameraOff"))}}}}; }
          async function refresh(selected){
            const list=(await navigator.mediaDevices.enumerateDevices()).filter(device=>
              device.kind==='videoinput'&&device.deviceId&&device.deviceId!=='default');
            devices.replaceChildren();
            if(!list.length) devices.add(new Option({{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamDefaultCamera"))}}}},''));
            list.forEach((device,index)=>
              devices.add(new Option(device.label||({{{{System.Text.Json.JsonSerializer.Serialize(Loc.Format("WebcamNumberedCamera", "{0}"))}}}}.replace('{0}', (index+1).toLocaleString({{{{System.Text.Json.JsonSerializer.Serialize(System.Globalization.CultureInfo.CurrentCulture.Name)}}}} || undefined))),device.deviceId)));
            if([...devices.options].some(option=>option.value===selected)) devices.value=selected;
          }
          async function openCamera(){
            const ticket=++generation, wanted=devices.value;
            release(); start.disabled=true; stop.disabled=false; status.textContent={{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamOpening"))}}}};
            try {
              const next=await navigator.mediaDevices.getUserMedia({audio:false,video:{
                ...(wanted?{deviceId:{exact:wanted}}:{}),width:{ideal:1920},height:{ideal:1080},frameRate:{ideal:30,max:60}}});
              if(ticket!==generation){next.getTracks().forEach(track=>track.stop());return;}
              stream=next; video.srcObject=next;
              const track=next.getVideoTracks()[0], actual=track.getSettings();
              status.textContent=actual.width&&actual.height?actual.width+' × '+actual.height:{{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamCameraOn"))}}}};
              track.addEventListener('ended',()=>{ if(ticket===generation){closeCamera();status.textContent={{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamDisconnected"))}}}};} });
              try { await refresh(actual.deviceId||wanted); } catch {}
            } catch(error) {
              if(ticket!==generation)return;
              closeCamera();
              status.textContent=error.name==='NotAllowedError'?{{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamAllowAccess"))}}}}:
                error.name==='NotFoundError'?{{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamNotFound"))}}}}:{{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamUnavailable"))}}}};
            }
          }
          start.onclick=openCamera; stop.onclick=closeCamera;
          devices.onchange=()=>{if(stream)openCamera();};
          window.addEventListener('pagehide',closeCamera);
          if(navigator.mediaDevices){
            refresh('').catch(()=>{});
            navigator.mediaDevices.addEventListener('devicechange',()=>refresh(devices.value).catch(()=>{}));
          }else{start.disabled=true;status.textContent={{{{System.Text.Json.JsonSerializer.Serialize(Loc.Get("WebcamServiceUnavailable"))}}}};}
        </script></body></html>
        """;
}
