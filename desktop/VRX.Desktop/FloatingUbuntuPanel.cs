using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using VRX.VrexBridge;
using Path = System.IO.Path;

namespace VRX.Desktop;

internal sealed class FloatingUbuntuPanel : IFloatingPanel, IFloatingTextTarget
{
    private readonly IVrexClient client;
    private readonly Action<IFloatingTextTarget> focus;
    private readonly UserControl surface;
    private readonly VrexSetup? setup;
    private readonly ScrollViewer? setupScroll;
    private readonly Action? openKeyboard;
    private readonly Button? setupToggle;
    private readonly Button? transferToggle;
    private readonly Button? textSync;
    private readonly StackPanel? transferControls;
    private readonly TextBlock transferStatus = new() { FontSize = 16, Margin = new(4, 0, 4, 3), TextWrapping = TextWrapping.Wrap };
    private bool textSharing;
    private readonly VrexVmIndicator vmIndicator = new();
    private readonly Button powerButton;
    private readonly Border stopConfirmation = new() { Visibility = Visibility.Collapsed };
    private readonly Button confirmStop, cancelStop;
    private readonly Image screen = new() { Stretch=Stretch.Uniform };
    private readonly Grid viewport = new() { Background=Brushes.Black,ClipToBounds=true };
    private readonly Canvas overlay = new() { IsHitTestVisible=false };
    private readonly VrexMarkCanvas annotations = new();
    private readonly Ellipse pointer = new() { Width=14,Height=14,Fill=Brushes.White,Stroke=Brushes.DarkCyan,StrokeThickness=2,Visibility=Visibility.Hidden };
    private readonly TextBlock status = new() { FontSize=15,TextWrapping=TextWrapping.Wrap,MaxHeight=42 };
    private readonly Dictionary<Button,Action> actions=[];
    private readonly RenderTargetBitmap bitmap = new(ControllerMenu.PixelWidth,ControllerMenu.PixelHeight,192,192,PixelFormats.Pbgra32);
    private readonly byte[] panelPixels = new byte[ControllerMenu.PixelBytes];
    private readonly DrawingVisual guestVisual = new();
    private RenderTargetBitmap? scaledGuest;
    private Int32Rect guestPixels;
    private WriteableBitmap? guestBitmap;
    private Button? held;
    private readonly PanelFeedback.Hover beamHover = new();
    private bool down,dirty=true, unpublishedFrame;
    private Point? lastBeamPoint;
    private long lastBeamMove;
    private int revision=-1,appearance=-1;
    public bool Closed {get;private set;}
    internal Point BotPoint {get;private set;}=new(-1,-1);
    internal bool ShowsSetup => setup?.Visibility == Visibility.Visible;
    private bool ChromeDirty => dirty || revision!=client.Revision || appearance!=Appearance.Revision;
    public bool Dirty => ChromeDirty || unpublishedFrame || client.FrameChanged;
    internal FloatingUbuntuPanel(Action<IFloatingTextTarget> focus, IVrexClient? client=null, Action? openKeyboard=null)
    {
        this.focus=focus;this.client=client??VrexClient.Current;
        this.openKeyboard=openKeyboard;
        var root=new DockPanel {Margin=new(48,48,48,12)};
        var title=new Border {Height=56,Background=Brushes.DarkSlateGray,Child=FloatingPanelChrome.Title(
            new TextBlock {Text=Loc.Get("VrexUbuntu"),FontSize=26,VerticalAlignment=VerticalAlignment.Center,Margin=new(12,0,0,0)},Button(Loc.Get("SnapshotClose"),Close))};
        DockPanel.SetDock(title,Dock.Top);root.Children.Add(title);
        var controls=new WrapPanel {Margin=new(0,3,0,3)};
        powerButton = Button("Start Ubuntu", () =>
        {
            if (CanStopUbuntu) ConfirmUbuntuStop();
            else this.client.Send(new("start"));
        });
        controls.Children.Add(powerButton);
        confirmStop = Button("Power off Ubuntu", () =>
        {
            DismissUbuntuStop();
            if (this.client is VrexClient worker) worker.StopUbuntu();
            else this.client.Stop();
        });
        cancelStop = Button("Keep Ubuntu running", DismissUbuntuStop);
        var stopActions = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Center };
        stopActions.Children.Add(confirmStop);
        stopActions.Children.Add(cancelStop);
        stopConfirmation.Background = new SolidColorBrush(Color.FromArgb(220, 16, 23, 39));
        stopConfirmation.Child = new StackPanel
        {
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Center,
            Children =
            {
                new TextBlock { Text = "Power off Ubuntu?", FontSize = 25, FontWeight = FontWeights.SemiBold,
                    Foreground = Brushes.White, HorizontalAlignment = HorizontalAlignment.Center },
                new TextBlock { Text = "Unsaved work in Ubuntu may be lost.", FontSize = 19, Foreground = Brushes.White,
                    HorizontalAlignment = HorizontalAlignment.Center, Margin = new(0, 6, 0, 12) },
                stopActions
            }
        };
        if(this.client is VrexClient)
        {
            setup = new VrexSetup(actions, ConfirmUbuntuStop) { Visibility = Visibility.Collapsed, Margin = new(12) };
            setup.SetValue(TextBlock.FontSizeProperty, 16.0);
            setupScroll = new ScrollViewer { Content = setup, Visibility = Visibility.Collapsed, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
            setupToggle = Button("Ubuntu setup", () => ShowSetup(setup.Visibility != Visibility.Visible));
            controls.Children.Add(setupToggle);
            transferControls = new StackPanel { Visibility = Visibility.Collapsed };
            var transfers = new WrapPanel();
            textSync = Button("Share text: Off", () =>
            {
                if (this.client.Send(new("clipboard-sync", Key: textSharing ? 0 : 1)))
                {
                    textSharing = !textSharing;
                    textSync!.Content = textSharing ? "Share text: On" : "Share text: Off";
                    transferStatus.Text = textSharing ? "Preparing text clipboard sharing…" : "Text clipboard sharing is off.";
                }
            });
            transfers.Children.Add(textSync);
            transfers.Children.Add(Button("Copy files to Ubuntu", () => this.client.Send(new("file-transfer", Key: 1))));
            transfers.Children.Add(Button("Copy files to Windows", () => this.client.Send(new("file-transfer"))));
            transferControls.Children.Add(transfers);
            transferControls.Children.Add(transferStatus);
            transferStatus.Text = "Copy files in Explorer or Ubuntu’s file manager first.";
            transferToggle = Button("Clipboard & files", () => transferControls.Visibility =
                transferControls.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible);
            controls.Children.Add(transferToggle);
        }
        DockPanel.SetDock(controls,Dock.Top);root.Children.Add(controls);
        if (transferControls != null)
        {
            DockPanel.SetDock(transferControls, Dock.Top);
            root.Children.Add(transferControls);
        }
        if (this.client is VrexClient)
        {
            DockPanel.SetDock(vmIndicator,Dock.Top);
            root.Children.Add(vmIndicator);
        }
        DockPanel.SetDock(status,Dock.Bottom);root.Children.Add(status);
        viewport.Children.Add(screen);viewport.Children.Add(annotations);overlay.Children.Add(pointer);viewport.Children.Add(overlay);root.Children.Add(viewport);
        if(setupScroll != null) viewport.Children.Add(setupScroll);
        viewport.Children.Add(stopConfirmation);
        surface=new UserControl {Width=ControllerMenu.Width,Height=ControllerMenu.Height,Content=FloatingPanelChrome.Wrap(root),FontFamily=new FontFamily("Segoe UI"),Foreground=Brushes.White};
        this.client.Changed+=OnEvent;this.client.EnsureWorker();RefreshVmStatus();
    }
    internal void ShowSetup(bool visible)
    {
        if(setup == null) return;
        setup.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        setupScroll!.Visibility = setup.Visibility;
        screen.Visibility = overlay.Visibility = annotations.Visibility = visible ? Visibility.Hidden : Visibility.Visible;
        setupToggle!.Content = visible ? "Ubuntu display" : "Ubuntu setup";
        if (down) { client.Send(new("input-cancel")); down = false; }
        if (visible) setup.Connect();
        else focus(this);
        dirty = true;
    }
    private Button Button(string label,Action action)
    {
        var button=new Button {Content=label,FontSize=17,Margin=new(3),Padding=new(10,7,10,7),MinHeight=38};
        actions[button]=action;return button;
    }
    private void OnEvent(VrexEvent message)
    {
        if (message.Type == "clipboard-sync" && textSync != null)
        {
            textSharing = message.X == 1;
            textSync.Content = textSharing ? "Share text: On" : "Share text: Off";
            transferStatus.Text = message.Text;
        }
        if (message.Type == "file-transfer") transferStatus.Text = message.Text;
        if (message is { Type: "screen-marks", Text: "ubuntu", Marks: { } marks })
            annotations.Update(marks, message.X == 1);
        if(message.Type is not ("pointer" or "click")) RefreshVmStatus();
        if(message.Type is "pointer" or "click")
        {
            double scale=Math.Min(viewport.ActualWidth/client.Width,viewport.ActualHeight/client.Height);
            Canvas.SetLeft(pointer,(viewport.ActualWidth-client.Width*scale)/2+message.X*scale-7);
            Canvas.SetTop(pointer,(viewport.ActualHeight-client.Height*scale)/2+message.Y*scale-7);pointer.Visibility=Visibility.Visible;
            var local=viewport.TranslatePoint(new Point(Canvas.GetLeft(pointer)+7,Canvas.GetTop(pointer)+7),surface);
            BotPoint=new Point(local.X/ControllerMenu.Width,local.Y/ControllerMenu.Height);
        }
        dirty=true;
    }
    private void RefreshVmStatus()
    {
        vmIndicator.Update(client);
        vmIndicator.Visibility = client.Ready && client.VmPhase==VrexVmPhase.Running ? Visibility.Collapsed : Visibility.Visible;
        bool canStop = CanStopUbuntu;
        powerButton.Content = canStop ? "Stop Ubuntu" : client.VmPhase == VrexVmPhase.Stopping ? "Stopping Ubuntu…" : "Start Ubuntu";
        powerButton.IsEnabled = canStop || (!client.Ready && client.VmPhase is VrexVmPhase.Off or VrexVmPhase.Failed);
        if (!canStop) DismissUbuntuStop();
        status.Text = client.Status == client.VmDetail ? "" : client.Status;
        status.Visibility = string.IsNullOrWhiteSpace(status.Text) ? Visibility.Collapsed : Visibility.Visible;
        if (!client.Ready || client.VmPhase == VrexVmPhase.DisplayUnavailable)
        {
            lastBeamPoint = null;
            annotations.Update([], true);
            guestBitmap = null;
            screen.Source = null;
            pointer.Visibility = Visibility.Hidden;
            BotPoint = new(-1, -1);
        }
        if (transferToggle != null)
        {
            transferToggle.IsEnabled = client.Ready;
            if (!client.Ready)
            {
                transferControls!.Visibility = Visibility.Collapsed;
                textSharing = false;
                textSync!.Content = "Share text: Off";
            }
        }
    }

    private bool CanStopUbuntu => client.VmPhase != VrexVmPhase.Stopping &&
        (client.Ready || client.VmPhase is VrexVmPhase.Preparing or VrexVmPhase.Booting);

    private void ConfirmUbuntuStop()
    {
        if (!CanStopUbuntu) return;
        if (down) { client.Send(new("input-cancel")); down = false; }
        beamHover.Set(null, () => dirty = true);
        stopConfirmation.Visibility = Visibility.Visible;
        dirty = true;
    }

    private void DismissUbuntuStop()
    {
        if (stopConfirmation.Visibility == Visibility.Collapsed) return;
        stopConfirmation.Visibility = Visibility.Collapsed;
        beamHover.Set(null, () => dirty = true);
        dirty = true;
    }

    private Button? HitButton(Point point)
    {
        DependencyObject? hit=VisualTreeHelper.HitTest(surface,point)?.VisualHit;
        while (hit != null)
        {
            if (hit is Button button && button.IsEnabled && actions.ContainsKey(button))
                return stopConfirmation.Visibility != Visibility.Visible || button == confirmStop || button == cancelStop ? button : null;
            hit = VisualTreeHelper.GetParent(hit);
        }
        return null;
    }
    internal static bool MapGuestPoint(Point point,Size viewportSize,int width,int height,out Point guest)
    {
        guest=default;
        if(width<1 || height<1 || viewportSize.Width<=0 || viewportSize.Height<=0)return false;
        double scale=Math.Min(viewportSize.Width/width,viewportSize.Height/height);
        double x=(point.X-(viewportSize.Width-width*scale)/2)/scale,y=(point.Y-(viewportSize.Height-height*scale)/2)/scale;
        if(x<0 || y<0 || x>=width || y>=height)return false;
        guest=new Point((int)x,(int)y);return true;
    }
    public void Pointer(int kind,float u,float v,float value=0)
    {
        if (kind is 4 or PanelFeedback.Leave)
        {
            lastBeamPoint = null;
            beamHover.Set(null, () => dirty = true);
        }
        if (kind == PanelFeedback.Leave) return;
        if(kind==4) {PanelFeedback.Cancel(held, () => dirty = true);held=null;down=false;client.Send(new("input-cancel"));return;}
        if(!float.IsFinite(u)||!float.IsFinite(v)||!float.IsFinite(value))return;
        var point=new Point(u*ControllerMenu.Width,v*ControllerMenu.Height);
        if (kind == 0) beamHover.Set(HitButton(point), () => dirty = true);
        if(kind==1)
        {
            held=HitButton(point);
            if (held != null) { PanelFeedback.Down(held, () => dirty = true); return; }
            if (stopConfirmation.Visibility == Visibility.Visible) return;
            if(setup?.Visibility == Visibility.Visible)
            {
                if(setup.SelectField(VisualTreeHelper.HitTest(surface,point)?.VisualHit))
                {
                    focus(this);
                    openKeyboard?.Invoke();
                    dirty = true;
                }
                return;
            }
            focus(this);
        }
        if(kind==2 && held!=null)
        {
            var button=held;held=null;
            if (HitButton(point)==button) { PanelFeedback.Up(button, () => dirty = true); actions[button](); }
            else PanelFeedback.Cancel(button, () => dirty = true);
            return;
        }
        if (stopConfirmation.Visibility == Visibility.Visible) return;
        if (ShowsSetup)
        {
            if (kind == 3 && setupScroll != null)
            {
                setupScroll.ScrollToVerticalOffset(setupScroll.VerticalOffset - value);
                dirty = true;
            }
            return;
        }
        var local=surface.TranslatePoint(point,viewport);
        if(client.Ready && MapGuestPoint(local,new Size(viewport.ActualWidth,viewport.ActualHeight),client.Width,client.Height,out var guest))
        {
            if (kind==0 && held==null && lastBeamPoint!=guest)
            {
                long now=Environment.TickCount64;
                if ((lastBeamPoint==null || now-lastBeamMove>=16) &&
                    client.Send(new("move",X:(int)guest.X,Y:(int)guest.Y)))
                {
                    lastBeamPoint=guest;
                    lastBeamMove=now;
                }
            }
            if(kind==1) {down=client.Send(new("down",X:(int)guest.X,Y:(int)guest.Y));}
            if(kind==2 && down) {client.Send(new("up",X:(int)guest.X,Y:(int)guest.Y));down=false;}
            if(kind==6)client.Send(new("right",X:(int)guest.X,Y:(int)guest.Y));
            if(kind==3)client.Send(new("scroll",X:(int)guest.X,Y:(int)guest.Y,Key:value>0?1:-1));
        }
        else
        {
            if (kind==0) lastBeamPoint=null;
            if (kind==2 && down) {client.Send(new("input-cancel"));down=false;}
        }
    }
    public void Edit(KeyboardEdit edit)
    {
        if (stopConfirmation.Visibility == Visibility.Visible) return;
        if(setup?.Visibility == Visibility.Visible) { setup.Edit(edit); dirty = true; return; }
        string Chord(string key)
        {
            string prefix="";
            if(edit.Modifiers.HasFlag(KeyboardModifiers.Control))prefix+="ctrl+";
            if(edit.Modifiers.HasFlag(KeyboardModifiers.Alt))prefix+="alt+";
            if(edit.Modifiers.HasFlag(KeyboardModifiers.Shift))prefix+="shift+";
            if(edit.Modifiers.HasFlag(KeyboardModifiers.Windows))prefix+="super+";
            return prefix+key;
        }
        for(int i=0;i<edit.Backspaces;i++)client.Send(new("key",Text:Chord("backspace")));
        if(edit.Text.Length>0)client.Send(new("text",edit.Text));
        string? key=edit.Key switch {8=>"backspace",9=>"tab",13=>"enter",27=>"escape",32=>"space",37=>"left",38=>"up",39=>"right",40=>"down",46=>"delete",>=65 and <=90=>((char)(edit.Key+32)).ToString(),>=48 and <=57=>((char)edit.Key).ToString(),_=>null};
        if(key!=null)client.Send(new("key",Text:Chord(key)));
    }
    internal Point? ProjectGuestPoint(VrexMarkPoint point)
    {
        if (guestPixels.IsEmpty || ShowsSetup) return null;
        return new Point((guestPixels.X + point.X * guestPixels.Width) / ControllerMenu.PixelWidth,
            (guestPixels.Y + point.Y * guestPixels.Height) / ControllerMenu.PixelHeight);
    }

    public byte[] Render()
    {
        var pixels = client.ReadFrame();
        if (pixels != null)
        {
            unpublishedFrame = true;
            if(guestBitmap==null || guestBitmap.PixelWidth!=client.Width || guestBitmap.PixelHeight!=client.Height)
            {
                guestBitmap=new WriteableBitmap(client.Width,client.Height,96,96,PixelFormats.Bgra32,null);
                screen.Source=guestBitmap;
                dirty=true;
            }
            guestBitmap.WritePixels(new Int32Rect(0,0,client.Width,client.Height),pixels,client.Width*4,0);
        }
        if (!ChromeDirty)
        {
            if (pixels == null || ShowsSetup || stopConfirmation.Visibility == Visibility.Visible) return panelPixels;
            if (!guestPixels.IsEmpty && !annotations.HasMarks && pointer.Visibility != Visibility.Visible)
            {
                RenderGuestPixels();
                return panelPixels;
            }
        }
        surface.Measure(new Size(ControllerMenu.Width,ControllerMenu.Height));surface.Arrange(new Rect(0,0,ControllerMenu.Width,ControllerMenu.Height));surface.UpdateLayout();
        guestPixels = Int32Rect.Empty;
        if (client.Width > 0 && client.Height > 0)
        {
            double scale = Math.Min(viewport.ActualWidth / client.Width, viewport.ActualHeight / client.Height);
            double width = client.Width * scale, height = client.Height * scale;
            annotations.ImageBounds = new Rect((viewport.ActualWidth - width) / 2, (viewport.ActualHeight - height) / 2, width, height);
            var origin = viewport.TranslatePoint(new Point((viewport.ActualWidth - width) / 2, (viewport.ActualHeight - height) / 2), surface);
            int left = Math.Clamp((int)Math.Round(origin.X * 2), 0, ControllerMenu.PixelWidth);
            int top = Math.Clamp((int)Math.Round(origin.Y * 2), 0, ControllerMenu.PixelHeight);
            int right = Math.Clamp((int)Math.Round((origin.X + width) * 2), left, ControllerMenu.PixelWidth);
            int bottom = Math.Clamp((int)Math.Round((origin.Y + height) * 2), top, ControllerMenu.PixelHeight);
            if (right > left && bottom > top) guestPixels = new(left, top, right-left, bottom-top);
        }
        bitmap.Clear();bitmap.Render(surface);bitmap.CopyPixels(panelPixels,ControllerMenu.PixelWidth*4,0);return panelPixels;
    }
    private void RenderGuestPixels()
    {
        // Only replace the guest image rectangle when a video frame arrives.
        // Controls, letterbox, and theme stay in the previously rendered texture.
        int width = guestPixels.Width, height = guestPixels.Height, stride = ControllerMenu.PixelWidth * 4;
        if (scaledGuest == null || scaledGuest.PixelWidth != width || scaledGuest.PixelHeight != height)
            scaledGuest = new(width,height,192,192,PixelFormats.Pbgra32);
        using (var drawing = guestVisual.RenderOpen())
            drawing.DrawImage(guestBitmap!, new Rect(0,0,width/2.0,height/2.0));
        scaledGuest.Render(guestVisual);
        scaledGuest.CopyPixels(Int32Rect.Empty, panelPixels, stride, guestPixels.Y * stride + guestPixels.X * 4);
    }
    public void Published() {dirty=false;unpublishedFrame=false;revision=client.Revision;appearance=Appearance.Revision;}
    internal static void SmokeTest(string output)
    {
        VrexModelSettings.SmokeSavedKey();
        static void Check(bool condition,string message) {if(!condition)throw new Exception("Ubuntu panel: "+message);}
        Check(MapGuestPoint(new Point(500,250),new Size(1000,500),1280,800,out var centre) && centre==new Point(640,400),"letterbox coordinate mapping");
        Check(!MapGuestPoint(new Point(10,250),new Size(1000,500),1280,800,out _),"letterbox must reject input");
        var fixture=new FixtureClient();
        var power = new FixtureClient();
        using (var powerPanel = new FloatingUbuntuPanel(_ => { }, power))
        {
            powerPanel.ClickForSmoke(powerPanel.powerButton);
            Check(power.StopCalls == 0 && powerPanel.stopConfirmation.Visibility == Visibility.Visible, "Stop opens a confirmation");
            powerPanel.ClickForSmoke(powerPanel.cancelStop);
            Check(power.StopCalls == 0 && powerPanel.stopConfirmation.Visibility == Visibility.Collapsed, "Cancelling leaves Ubuntu running");
            powerPanel.ClickForSmoke(powerPanel.powerButton);
            powerPanel.ClickForSmoke(powerPanel.confirmStop);
            Check(power.StopCalls == 1, "Confirming powers off Ubuntu");
            power.VmPhase = VrexVmPhase.Stopping;
            power.Emit(new("vm-status"));
            Check(!powerPanel.powerButton.IsEnabled, "Start waits for Ubuntu shutdown");
            power.Ready = false;
            power.VmPhase = VrexVmPhase.Off;
            power.Emit(new("ubuntu-stopped"));
            powerPanel.ClickForSmoke(powerPanel.powerButton);
            Check(power.Requests.LastOrDefault()?.Type == "start", "The same panel can start Ubuntu again");
        }

        using var panel=new FloatingUbuntuPanel(_=>{},fixture);
        var pixels=panel.Render().ToArray();panel.Published();
        fixture.Green = 206;
        var refreshed = panel.Render();
        int guestCentre = ((panel.guestPixels.Y + panel.guestPixels.Height / 2) * ControllerMenu.PixelWidth +
            panel.guestPixels.X + panel.guestPixels.Width / 2) * 4;
        Check(panel.scaledGuest != null && refreshed[guestCentre + 1] == 206 && refreshed.AsSpan(0, ControllerMenu.PixelWidth*4)
            .SequenceEqual(pixels.AsSpan(0, ControllerMenu.PixelWidth*4)),"live frames update the image without repainting panel chrome");
        fixture.ReadEnabled = false;
        Check(panel.Dirty && ReferenceEquals(panel.Render(), refreshed),"a frame stays ready to publish if the shared lock was busy");
        panel.Published();
        Check(!panel.Dirty,"publishing clears the pending frame");
        fixture.ReadEnabled = true;
        fixture.Green = null;
        var local=panel.viewport.TranslatePoint(new Point(panel.viewport.ActualWidth/2,panel.viewport.ActualHeight/2),panel.surface);
        float u=(float)(local.X/ControllerMenu.Width),v=(float)(local.Y/ControllerMenu.Height);
        panel.Pointer(0,u,v);
        Check(fixture.Requests[^1].Type=="move" && Math.Abs(fixture.Requests[^1].X-640)<=1 &&
            Math.Abs(fixture.Requests[^1].Y-400)<=1,"beam hover moves the guest cursor");
        int moves=fixture.Requests.Count;
        panel.Pointer(0,u,v);
        panel.Pointer(0,0,0);
        Check(fixture.Requests.Count==moves,"stationary beam and positions outside the guest do not send movement");
        fixture.Emit(new("screen-marks", "ubuntu", X: 1, Marks:
            [new("Box", [new(.25,.25),new(.75,.75)], null, "#F43F5E")]));
        Check(!panel.Render().AsSpan().SequenceEqual(pixels),"Ubuntu marks render on the panel");
        fixture.Emit(new("screen-marks", "ubuntu", X: 1, Marks: []));
        Check(panel.Render().AsSpan().SequenceEqual(pixels),"clearing Ubuntu marks restores the underlying image");
        panel.Pointer(1,u,v);panel.Pointer(2,u,v);
        Check(fixture.Requests.TakeLast(2).Select(r=>r.Type).SequenceEqual(new[]{"down","up"}),"trigger click routing");
        Check(panel.viewport.ActualHeight > 350,"Ubuntu display expands into available panel space");
        Check(Math.Abs(fixture.Requests[^1].X-640)<=1 && Math.Abs(fixture.Requests[^1].Y-400)<=1,"screen coordinates");
        panel.Pointer(1,u,v);panel.Pointer(4,0,0);
        Check(fixture.Requests[^1].Type=="input-cancel","focus loss cancels drag");
        panel.Pointer(6,u,v);panel.Pointer(3,u,v,240);
        Check(fixture.Requests[^2].Type=="right" && fixture.Requests[^1].Type=="scroll" && fixture.Requests[^1].Key==1,"context and scroll routing");
        panel.Edit(new KeyboardEdit("hello",0,0,0));
        Check(fixture.Requests[^1].Type=="text" && fixture.Requests[^1].Text=="hello","guest keyboard routing");
        panel.Edit(new KeyboardEdit("",65,Modifiers:KeyboardModifiers.Control));
        Check(fixture.Requests[^1].Type=="key" && fixture.Requests[^1].Text=="ctrl+a","guest keyboard modifiers");
        fixture.Emit(new("reply","Hello, I am Vrex. I can help inside Ubuntu."));
        fixture.Emit(new("pointer",X:640,Y:400));
        Check(panel.Dirty && panel.BotPoint.X>0 && panel.BotPoint.Y>0,"assistant replies and pointer invalidate surface");
        FloatingVrexGridPanel.SmokeTest(fixture, fixture.Emit);
        using var chat=new FloatingVrexChatPanel(fixture, _=>{}, ()=>{});
        chat.VerifyActivityIndicator(state => fixture.State = state);
        chat.VerifyLiveTranscript(fixture.Emit);
        chat.Edit(new KeyboardEdit("Open Settings"));
        chat.Edit(new KeyboardEdit("",13));
        Check(fixture.Requests[^1].Type=="chat" && fixture.Requests[^1].Text=="Open Settings","independent chat keyboard and send");
        for (int i = 0; i < 24; ++i) fixture.Emit(new("reply", $"Vrex chat line {i}"));
        chat.VerifyTranscriptDrag();
        chat.VerifyStopClick();
        Check(fixture.Requests[^1].Type == "chat-stop", "chat Stop routes a cancellation request");
        chat.VerifyGoalBanner(fixture.Emit);
        Check(fixture.Requests.Any(request => request is { Type: "goal-detail", Text: "running" }),
            "The goal banner opens details for the running goal");
        pixels=panel.Render();
        var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(BitmapSource.Create(ControllerMenu.PixelWidth,ControllerMenu.PixelHeight,192,192,PixelFormats.Pbgra32,null,pixels,ControllerMenu.PixelWidth*4)));
        using(var file=File.Create(Path.Combine(output,"floating-ubuntu.png")))png.Save(file);
        File.WriteAllText(Path.Combine(output,"vrex-panel-pass.txt"),"PASS: letterboxing, guest clicks, focus cancellation, context click, scrolling, keyboard, chat and assistant pointer.\n");
        using var caption=new FloatingVrexReply();caption.Show("I am working in your Ubuntu VM.");
        var captionPixels=caption.Render();caption.Published();
        int cardAlpha = ((ControllerMenu.PixelHeight - 136) * ControllerMenu.PixelWidth + ControllerMenu.PixelWidth / 2) * 4 + 3;
        Check(!caption.Closed && !caption.Dirty && captionPixels[3]==0 && captionPixels[cardAlpha] is > 0 and < 255,
            "Vrex reply caption transparency and lifecycle");
        var captionPng=new PngBitmapEncoder();captionPng.Frames.Add(BitmapFrame.Create(BitmapSource.Create(ControllerMenu.PixelWidth,ControllerMenu.PixelHeight,96,96,PixelFormats.Pbgra32,null,captionPixels,ControllerMenu.PixelWidth*4)));
        using(var file=File.Create(Path.Combine(output,"vrex-reply.png")))captionPng.Save(file);
    }
    private sealed class FixtureClient : IVrexClient
    {
        public VrexVmPhase VmPhase { get; set; } = VrexVmPhase.Running;
        public string VmDetail => Status;
        public int Width=>1280;public int Height=>800;public string Status=>"Ubuntu fixture — Vrex ready";public string State { get; set; } = "idle";
        public bool Ready { get; set; } = true;
        public int Revision=>0;public bool FrameChanged=>false;
        public event Action<VrexEvent>? Changed;
        public readonly List<VrexRequest> Requests=[];
        public int StopCalls { get; private set; }
        public void EnsureWorker() {} public void Stop() => StopCalls++;
        public bool Send(VrexRequest request) {Requests.Add(request);return true;}
        public void Emit(VrexEvent message)=>Changed?.Invoke(message);
        internal byte? Green { get; set; }
        internal bool ReadEnabled { get; set; } = true;
        public byte[]? ReadFrame()
        {
            if (!ReadEnabled) return null;
            byte[] pixels=new byte[Width*Height*4];
            for(int y=0;y<Height;y++)for(int x=0;x<Width;x++) {int i=(y*Width+x)*4;pixels[i]=(byte)(60+x/16);pixels[i+1]=Green ?? (byte)(50+y/8);pixels[i+2]=25;pixels[i+3]=255;}
            return pixels;
        }
    }
    private void ClickForSmoke(Button button)
    {
        Render();
        var point = button.TranslatePoint(new Point(button.ActualWidth / 2, button.ActualHeight / 2), surface);
        float u = (float)(point.X / ControllerMenu.Width), v = (float)(point.Y / ControllerMenu.Height);
        Pointer(1, u, v);
        Pointer(2, u, v);
    }
    private void Close()
    {
        if (textSharing && client.Ready) client.Send(new("clipboard-sync"));
        Closed = true;
        textSharing = false;
    }
    public void Dispose() {Close();setup?.Dispose();client.Changed-=OnEvent;if(down)client.Send(new("input-cancel"));}
}
