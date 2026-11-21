namespace PersonaBot;

/// <summary>
/// The product's public name. Code, folders, the data directory and saved sessions keep the
/// internal name PersonaBot; everything a person reads (window titles, the chat, the AI's own
/// introduction) uses this.
/// </summary>
public static class Brand
{
    public const string Name = "PersonaBot";
    /// <summary>The window's title: the name alone (the bots do more than test websites now).</summary>
    public const string WindowTitle = Name;

    /// <summary>PersonaBot's tagline (chosen 16 Nov 2026), also on personabot.uk.</summary>
    public const string Tagline = "The Next Generation AI Worker";
}
