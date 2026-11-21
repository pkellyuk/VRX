namespace PersonaBot;

/// <summary>
/// The names PersonaBot gives things inside its Linux machines: the desktop user of a Debian or Ubuntu machine,
/// its state folder, the Firefox profile, the markers the guest writes on the serial console, and the tags of
/// saved checkpoints.
/// </summary>
public static class GuestNames
{
    /// <summary>The desktop user of a Debian or Ubuntu machine.</summary>
    public const string DesktopUser = "personabot";

    /// <summary>That user's home folder.</summary>
    public const string DesktopHome = "/home/" + DesktopUser;

    /// <summary>
    /// Checks a desktop user name before it goes into a guest script (a machine's <see cref="Machine.DesktopUser"/>
    /// is either <see cref="DesktopUser"/> or the person's account): 2 to 24 small letters or digits, starting with a letter.
    /// </summary>
    public static string CheckUser(string user)
    {
        ArgumentNullException.ThrowIfNull(user);
        if (!System.Text.RegularExpressions.Regex.IsMatch(user, "^[a-z][a-z0-9]{1,23}$"))
        {
            System.Diagnostics.Trace.TraceWarning($"GuestNames.CheckUser: refused '{user}'");
            throw new ArgumentException($"'{user}' isn't a Linux user name PersonaBot uses.", nameof(user));
        }
        return user;
    }

    /// <summary>PersonaBot's own folder in a machine (setup markers, Tiny Core's X configuration, Android's scripts).</summary>
    public const string StateFolder = "/opt/personabot";

    /// <summary>The file first-boot setup leaves once it has finished.</summary>
    public const string ProvisionedMarker = StateFolder + "/provisioned";

    /// <summary>The name of the Firefox profile PersonaBot's browser opens.</summary>
    public const string BrowserProfileName = "personabot";

    /// <summary>The Firefox profile of a home folder.</summary>
    public static string BrowserProfile(string home)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(home);
        return home.TrimEnd('/') + "/.mozilla/firefox/" + BrowserProfileName;
    }

    /// <summary>What a machine writes on its serial console once its desktop and Firefox are up.</summary>
    public const string BootReady = "PERSONABOT_BOOT_READY";

    /// <summary>What first-boot setup writes on the serial console when it fails.</summary>
    public const string ProvisionFailed = "PERSONABOT_PROVISION_FAILED";

    /// <summary>What Tiny Core's image build writes when it has finished.</summary>
    public const string ProvisionSucceeded = "PERSONABOT_PROVISION_SUCCESS";

    /// <summary>The start of every checkpoint tag PersonaBot writes; 32 hex digits follow.</summary>
    public const string CheckpointTagPrefix = "personabot-";

    /// <summary>Whether a boot log says the desktop is ready.</summary>
    public static bool SaysBootReady(string bootLog)
    {
        ArgumentNullException.ThrowIfNull(bootLog);
        return bootLog.Contains(BootReady, StringComparison.Ordinal);
    }

    /// <summary>Whether a boot log says first-boot setup failed.</summary>
    public static bool SaysProvisionFailed(string bootLog)
    {
        ArgumentNullException.ThrowIfNull(bootLog);
        return bootLog.Contains(ProvisionFailed, StringComparison.Ordinal);
    }

    /// <summary>Whether a tag is one of PersonaBot's checkpoints: the prefix, then 32 hex digits.</summary>
    public static bool IsCheckpointTag(string? tag) =>
        tag != null && System.Text.RegularExpressions.Regex.IsMatch(tag, "^" + CheckpointTagPrefix + "[a-f0-9]{32}$");

    /// <summary>A new checkpoint tag.</summary>
    public static string NewCheckpointTag() => CheckpointTagPrefix + Guid.NewGuid().ToString("N");
}
