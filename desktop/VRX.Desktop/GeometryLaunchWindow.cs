using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace VRX.Desktop;

// A concrete restart action with the original command line already filled in.
// No game process or file is changed while this dialog is open.
public sealed class GeometryLaunchWindow : Window
{
    public GeometryLaunch.Plan Plan { get; private set; }
    public float Separation { get; private set; }
    public float Convergence { get; private set; }
    public GeometryLaunchWindow(GeometryLaunch.Plan plan,Profile profile)
    {
        Plan=plan; Title=Loc.Format(plan.App.Pid==0?"GeometryLaunchTitle":"GeometryRestartTitle",StereoPaths.Label(plan.Backend));
        Width=620; SizeToContent=SizeToContent.Height; MaxHeight=Math.Min(900,SystemParameters.WorkArea.Height-40); ResizeMode=ResizeMode.NoResize;
        WindowStartupLocation=WindowStartupLocation.CenterOwner; ShowInTaskbar=false;
        Background=(Brush)Application.Current.FindResource("Page"); Foreground=(Brush)Application.Current.FindResource("Ink");
        var body=new StackPanel{Margin=new Thickness(24)};
        Content=new ScrollViewer{Content=body,VerticalScrollBarVisibility=ScrollBarVisibility.Auto};
        body.Children.Add(new TextBlock{Text=Loc.Format(plan.App.Pid==0?"GeometryLaunchIntro":"GeometryRestartIntro",MainWindow.GameDisplayName(plan.App.Windows.FirstOrDefault()?.Title,plan.App.Name),StereoPaths.Label(plan.Backend)),TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,0,12),FontSize=16});
        body.Children.Add(new TextBlock{Text=Loc.Get(plan.App.Pid==0?"GeometryLaunchInfo":"GeometryRestartSave")+"\n\n"+Loc.Get("GeometryChooserNotice"),TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,0,12)});
        body.Children.Add(new TextBlock{Text=plan.App.FullPath,TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,0,12),FontSize=12});
        TextBox Field(string label,string value,bool multi=false) {
            body.Children.Add(new TextBlock{Text=label,Margin=new Thickness(0,8,0,4)});
            var field=new TextBox{Text=value,MinHeight=multi?70:30,AcceptsReturn=multi,TextWrapping=multi?TextWrapping.Wrap:TextWrapping.NoWrap,MaxLength=32768};
            System.Windows.Automation.AutomationProperties.SetName(field,label); body.Children.Add(field); return field;
        }
        var arguments=Field(Loc.Get("GeometryArguments"),string.Join(' ',plan.Arguments.Select(GeometryLaunch.QuoteArgument)),true);
        body.Children.Add(new TextBlock{Text=Loc.Get("GeometryArgumentsHint"),TextWrapping=TextWrapping.Wrap,FontSize=12,Margin=new Thickness(0,4,0,0)});
        var working=Field(Loc.Get("GeometryWorkingFolder"),plan.WorkingDirectory);
        var steam=Field(Loc.Get("GeometrySteamId"),plan.SteamAppId);
        var separation=Field(Loc.Get("GeometrySeparation"),profile.GeometrySeparation.ToString("R",CultureInfo.CurrentCulture));
        var convergence=Field(Loc.Get("GeometryConvergence"),profile.GeometryConvergence.ToString("R",CultureInfo.CurrentCulture));
        var error=new TextBlock{TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,12,0,0)}; body.Children.Add(error);
        var buttons=new StackPanel{Orientation=Orientation.Horizontal,HorizontalAlignment=HorizontalAlignment.Right,Margin=new Thickness(0,16,0,0)};
        var cancel=new Button{Content=Loc.Get("GeometryCancel"),IsCancel=true,Padding=new Thickness(14,8,14,8),Margin=new Thickness(0,0,8,0)};
        var restart=new Button{Content=Loc.Get(plan.App.Pid==0?"GeometryLaunchAction":"GeometryRestartAction"),Padding=new Thickness(14,8,14,8)};
        buttons.Children.Add(cancel); buttons.Children.Add(restart); body.Children.Add(buttons);
        cancel.Click+=(_,_)=>DialogResult=false;
        restart.Click+=(_,_)=>{
            try {
                if(!Directory.Exists(working.Text)) throw new InvalidDataException(Loc.Get("ErrorGeometryFolder"));
                if(steam.Text.Any(c=>!char.IsAsciiDigit(c))) throw new InvalidDataException(Loc.Get("ErrorGeometrySteamId"));
                if(!float.TryParse(separation.Text,NumberStyles.Float,CultureInfo.CurrentCulture,out float shift) ||
                    !float.TryParse(convergence.Text,NumberStyles.Float,CultureInfo.CurrentCulture,out float anchor) || !Profile.SafeGeometryTuning(shift,anchor))
                    throw new InvalidDataException(Loc.Get("ErrorGeometryTuning"));
                Plan=plan with {Arguments=GeometryLaunch.ParseArguments(arguments.Text),WorkingDirectory=Path.GetFullPath(working.Text),SteamAppId=steam.Text};
                Separation=shift; Convergence=anchor; DialogResult=true;
            } catch(Exception ex) { error.Text=ex.Message; }
        };
    }
}
