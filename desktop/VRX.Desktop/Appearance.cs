using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Effects;

namespace VRX.Desktop;

internal static class Appearance
{
    internal sealed record Palette(string Key, string Icon, string Accent, string Page, string Card, string Ink, string Muted);
    internal static readonly Palette[] Themes =
    [
        new("ThemeDefault", "✦", "#76DCC6", "#101827", "#192438", "#E8EEF8", "#9EAFC7"),
        new("ThemeRobots", "🤖", "#61D9EF", "#101C29", "#1B3042", "#E7F8FF", "#98BACA"),
        new("ThemeDinosaurs", "🦖", "#9FE1A0", "#14221C", "#233A2E", "#EFF8E9", "#ACC1A7"),
        new("ThemeAquarium", "🐠", "#67DDFA", "#0C1D2C", "#14364A", "#E0F8FF", "#96BECE"),
        new("ThemeHell", "🔥", "#FFAB79", "#27191C", "#42262B", "#FFF1E7", "#D3ADA3"),
        new("ThemeHeaven", "☁", "#547EC0", "#EAF2FC", "#FBFDFF", "#183258", "#596F90"),
        new("ThemeDark", "◐", "#B7C5DE", "#101114", "#1C1E24", "#EDEFF5", "#A1A7B3"),
        new("ThemeLight", "☀", "#367D95", "#EFF2F6", "#FFFFFF", "#182536", "#5B6A7C"),
        new("ThemeGrape", "🍇", "#D0AEFF", "#201A2E", "#332843", "#F4EBFF", "#BBACCF"),
    ];
    private static readonly List<WeakReference<Grid>> panels = [];
    private static int floatingTheme = AppSettings.DefaultFloatingTheme;
    private static double opacity = AppSettings.DefaultFloatingOpacity / 100.0;
    internal static int Revision { get; private set; }
    private static Palette Floating => Themes[floatingTheme];
    internal static Palette FloatingPalette => Floating;
    internal static SolidColorBrush Brush(string hex, double alpha = 1)
    {
        var colour = (Color)ColorConverter.ConvertFromString(hex);
        colour.A = (byte)Math.Round(Math.Clamp(alpha, 0, 1) * 255);
        return new SolidColorBrush(colour);
    }
    private static string Mix(string a, string b, double amount)
    {
        var ca = (Color)ColorConverter.ConvertFromString(a); var cb = (Color)ColorConverter.ConvertFromString(b);
        return $"#{(byte)(ca.R + (cb.R-ca.R)*amount):X2}{(byte)(ca.G + (cb.G-ca.G)*amount):X2}{(byte)(ca.B + (cb.B-ca.B)*amount):X2}";
    }
    internal static void Configure(int panelTheme, int panelOpacity, int desktopTheme)
    {
        floatingTheme = Math.Clamp(panelTheme, 0, Themes.Length - 1);
        opacity = Math.Clamp(panelOpacity, 0, 100) / 100.0;
        Revision++;
        foreach (var weak in panels.ToArray())
            if (weak.TryGetTarget(out var panel)) StylePanel(panel); else panels.Remove(weak);
        var desktop = Themes[Math.Clamp(desktopTheme, 0, Themes.Length - 1)];
        // The desktop keeps its restrained layout; only its material colours change.
        foreach (var (key, value) in new Dictionary<string, string>
        {
            ["Page"] = desktop.Page, ["Card"] = desktop.Card, ["CardEdge"] = Mix(desktop.Card, desktop.Accent, .16),
            ["Ink"] = desktop.Ink, ["Muted"] = desktop.Muted, ["Soft"] = desktop.Muted,
            ["Accent"] = desktop.Accent, ["Well"] = desktop.Page, ["Field"] = desktop.Page,
            ["Raised"] = desktop.Card, ["RaisedEdge"] = Mix(desktop.Card, desktop.Accent, .22), ["Hover"] = Mix(desktop.Card, desktop.Accent, .10),
            ["ChromeBar"] = desktop.Page, ["ChromeText"] = desktop.Ink, ["ChromeEdge"] = desktop.Card,
            ["WindowEdge"] = desktop.Card, ["CaptionHover"] = desktop.Card, ["CaptionPressed"] = desktop.Muted,
            ["OnAccent"] = "#102433"
        }) Application.Current.Resources[key] = Brush(value);
    }
    internal static void Register(Grid root) { panels.Add(new(root)); StylePanel(root); }
    private static void StylePanel(Grid root)
    {
        var theme = Floating;
        root.Resources["Accent"] = Brush(theme.Accent);
        var accent = ((SolidColorBrush)root.Resources["Accent"]).Color;
        double luminance = .2126 * accent.ScR + .7152 * accent.ScG + .0722 * accent.ScB;
        root.Resources["OnPanelAccent"] = luminance >= .18 ? Brushes.Black : Brushes.White;
        root.Resources["Ink"] = Brush(theme.Ink); root.Resources["Muted"] = Brush(theme.Muted);
        root.Resources["Card"] = Brush(theme.Card, opacity); root.Resources["Page"] = Brush(theme.Page, opacity);
        root.Resources["Well"] = Brush(theme.Page, opacity); root.Resources["Field"] = Brush(theme.Page, opacity);
        root.SetValue(TextElement.ForegroundProperty, Brush(theme.Ink));
        root.Resources[typeof(TextBlock)] = new Style(typeof(TextBlock)) { Setters = { new Setter(TextBlock.ForegroundProperty, Brush(theme.Ink)) } };
        root.Resources[typeof(Button)] = ButtonStyle(theme);
        root.Resources[typeof(CheckBox)] = ToggleStyle(theme);
        root.Resources[typeof(Slider)] = new Style(typeof(Slider), (Style)Application.Current.FindResource(typeof(Slider)))
        { Setters = { new Setter(Control.ForegroundProperty, Brush(theme.Accent)) } };
        void Visit(DependencyObject node)
        {
            if (node is TextBlock text)
            {
                text.Foreground = Brush(theme.Ink);
                if (text.Tag as string == "ThemeDecoration") { text.Text = theme.Icon; text.Foreground = Brush(theme.Accent, .10); }
            }
            if (node is Control control && control is not Button && control is not CheckBox) control.Foreground = Brush(theme.Ink);
            if (node is Button button && button.ReadLocalValue(Control.BackgroundProperty) is SolidColorBrush buttonFill && buttonFill.Color != Colors.Transparent)
            {bool selected = buttonFill.Color == Colors.DarkCyan;button.Background = Brush(selected ? theme.Accent : theme.Card, Math.Max(.12, opacity));button.Foreground = Brush(selected ? "#102433" : theme.Ink);}
            if (node is Border border)
            {
                if (border.Tag as string == "GlassFrame")
                {
                    border.Background = new LinearGradientBrush(((SolidColorBrush)Brush(theme.Card, opacity)).Color,
                        ((SolidColorBrush)Brush(theme.Page, opacity)).Color, 90);
                    border.BorderBrush = Brush(theme.Accent, .45);
                }
                else if (border.Tag as string == "GlassGlow") border.BorderBrush = Brush(theme.Accent, .12);
                else if (border.Tag as string == "ScrollPosition") border.Background = Brush(theme.Accent);
                else if (border.Background is SolidColorBrush fill && fill.Color != Colors.Transparent)
                    border.Background = Brush(theme.Card, Math.Max(.05, opacity * .8));
            }
            if (node is Panel panel && panel.Background is SolidColorBrush background && background.Color != Colors.Transparent)
                panel.Background = Brush(theme.Page, opacity);
            foreach (object child in LogicalTreeHelper.GetChildren(node)) if (child is DependencyObject item) Visit(item);
        }
        Visit(root);
    }
    private static Style ButtonStyle(Palette p)
    {
        var style = new Style(typeof(Button));
        style.Setters.Add(new Setter(Control.ForegroundProperty, Brush(p.Ink)));
        style.Setters.Add(new Setter(Control.BackgroundProperty, Brush(p.Card, Math.Max(.08, opacity * .7))));
        style.Setters.Add(new Setter(Control.BorderBrushProperty, Brush(p.Accent, .22)));
        style.Setters.Add(new Setter(Control.FontFamilyProperty, new FontFamily("Segoe UI")));
        style.Setters.Add(new Setter(Control.TemplateProperty, (ControlTemplate)XamlReader.Parse("""
            <ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TargetType="Button">
             <Border x:Name="Card" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1" CornerRadius="14" Padding="{TemplateBinding Padding}">
              <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition/></Grid.ColumnDefinitions>
               <TextBlock x:Name="ActionIcon" Text="{TemplateBinding Tag}" FontFamily="Segoe UI Emoji" FontSize="22" VerticalAlignment="Center" Margin="0,0,10,0"/>
               <ContentPresenter Grid.Column="1" VerticalAlignment="Center" HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}" RecognizesAccessKey="True"/>
              </Grid>
             </Border>
             <ControlTemplate.Triggers>
              <Trigger Property="Tag" Value="{x:Null}"><Setter TargetName="ActionIcon" Property="Visibility" Value="Collapsed"/></Trigger>
              <Trigger Property="Tag" Value=""><Setter TargetName="ActionIcon" Property="Visibility" Value="Collapsed"/></Trigger>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Card" Property="Opacity" Value=".82"/></Trigger>
              <Trigger Property="IsPressed" Value="True"><Setter TargetName="Card" Property="Opacity" Value=".65"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter TargetName="Card" Property="Opacity" Value=".42"/></Trigger>
             </ControlTemplate.Triggers>
            </ControlTemplate>
            """)));
        return style;
    }
    private static Style ToggleStyle(Palette p)
    {
        var style = new Style(typeof(CheckBox));
        style.Setters.Add(new Setter(Control.ForegroundProperty, Brush(p.Ink)));
        style.Setters.Add(new Setter(Control.BackgroundProperty, Brush(p.Accent)));
        style.Setters.Add(new Setter(Control.MinHeightProperty, 38.0));
        style.Setters.Add(new Setter(Control.TemplateProperty, (ControlTemplate)XamlReader.Parse("""
            <ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TargetType="CheckBox">
             <Grid Margin="2,4" Background="Transparent"><Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition Width="52"/></Grid.ColumnDefinitions>
              <ContentPresenter VerticalAlignment="Center" Margin="0,0,9,0" RecognizesAccessKey="True"/>
              <Border x:Name="Track" Grid.Column="1" Width="42" Height="25" Background="#40506A" CornerRadius="13" VerticalAlignment="Center">
               <Ellipse x:Name="Thumb" Width="19" Height="19" Fill="White" HorizontalAlignment="Left" Margin="3,0,0,0"/>
              </Border>
             </Grid>
             <ControlTemplate.Triggers>
              <Trigger Property="IsChecked" Value="True"><Setter TargetName="Track" Property="Background" Value="{Binding Background, RelativeSource={RelativeSource TemplatedParent}}"/><Setter TargetName="Thumb" Property="HorizontalAlignment" Value="Right"/><Setter TargetName="Thumb" Property="Margin" Value="0,0,3,0"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value=".4"/></Trigger>
             </ControlTemplate.Triggers>
            </ControlTemplate>
            """)));
        return style;
    }
    internal static string ActionIcon(string label)
    {
        if (label == Loc.Get("GameHelpStart")) return "?";
        if (label == Loc.Get("GameHelpSettings")) return "◉";
        if (label == Loc.Get("ControllerMenuBack")) return "‹";
        if (label == Loc.Get("RecenterNow")) return "⌖";
        if (label == Loc.Get("ControllerMenuControlScreen")) return "↗";
        if (label == Loc.Get("SnapshotTake")) return "▧";
        if (label == Loc.Get("SnapshotOpenFolder")) return "▱";
        if (label == Loc.Get("BrowserOpen")) return "◎";
        if (label == Loc.Get("KeyboardOpen")) return "⌨";
        if (label == Loc.Get("ArrangeWindows")) return "▦";
        foreach (var theme in Themes) if (label == Loc.Get(theme.Key)) return theme.Icon;
        return "";
    }
    internal static string Icon(string name) => name switch
    {
        "ThemesSection" => "◈", "GameSection" => "🎮", "ScreenSection" => "▣", "AroundSection" => "✧", "RoomSection" => "⌂",
        "RoomEnvironmentCategory" => "⌂", "RoomSurfacesCategory" => "☀", "RoomMusicCategory" => "♫",
        "RoomCreaturesCategory" => "🐟", "RoomVrexCategory" => "🤖", "RoomAtmosphereCategory" => "✧",
        "DepthSection" => "◈", "ExperimentalSection" => "⚗", "SteamVrSection" => "⚙", "ProfileSection" => "▤", _ => "✦"
    };
}
