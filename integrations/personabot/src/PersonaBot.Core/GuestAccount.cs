using System.Diagnostics;
using System.Globalization;

namespace PersonaBot;

/// <summary>How a guest creates users and switches to them.</summary>
public enum GuestAccountStyle
{
    /// <summary>Tiny Core Linux (Windows): busybox adduser; /etc/passwd lives in RAM, so users are recreated each boot with fixed ids.</summary>
    TinyCore,

    /// <summary>Debian in a full VM (Mac): useradd; users persist on disk.</summary>
    Debian,

    /// <summary>Debian under proot (Android): one app UID, so every persona really runs as root with its own home folder (organisational only).</summary>
    SharedRoot,

    /// <summary>Android guest: app sandboxes remain Android-owned; bot work folders share the debug shell.</summary>
    Android
}

/// <summary>
/// A persona's Linux account in the guest (PERSONAS.md §3, PRODUCT.md §1.4): its user, home folder and
/// own Firefox profile. The bash, read and write tools run as this user, not root, so the bots' files
/// and browser logins stay apart. The account is created on first use and again whenever the guest has
/// forgotten it (a Tiny Core reboot or an older snapshot).
/// </summary>
public sealed class GuestAccount
{
    /// <summary>Printed, with exit code 97, by a wrapped command when the account still has to be set up.</summary>
    public const string MissingMarker = "PERSONABOT_ACCOUNT_MISSING";

    public const int MissingExitCode = 97;

    /// <summary>Helper scripts on the guest's persistent disk.</summary>
    public const string BinFolder = "/opt/personabot/bin";

    public const string BrowserHelper = BinFolder + "/personabot-browser";

    private GuestAccount(string user, int uid, string home, bool isDesktopUser, GuestProfile profile, string displayName, bool fullSudo = false)
    {
        FullSudo = fullSudo;
        DisplayName = displayName;
        User = user;
        Uid = uid;
        Home = home;
        IsDesktopUser = isDesktopUser;
        Profile = profile;
    }

    public string User { get; }

    /// <summary>The persona's name, for its terminal window's title.</summary>
    public string DisplayName { get; }

    public int Uid { get; }

    public string Home { get; }

    /// <summary>True for the guest's own desktop user (the Website Tester): nothing is created for it.</summary>
    public bool IsDesktopUser { get; }

    public GuestProfile Profile { get; }

    /// <summary>Whether the persona may run anything as root with sudo (its editor's "Full sudo"); otherwise only apt.</summary>
    public bool FullSudo { get; }

    /// <summary>The sudo rule a Debian account gets (in /etc/sudoers.d/personabot-{user}).</summary>
    public string SudoRule => FullSudo
        ? $"{User} ALL=(ALL:ALL) NOPASSWD: ALL"
        : $"{User} ALL=(root) NOPASSWD: /usr/bin/apt-get update, /usr/bin/apt-get install *";

    /// <summary>The file that holds <see cref="SudoRule"/>.</summary>
    public string SudoFile => "/etc/sudoers.d/personabot-" + User;

    public GuestAccountStyle Style => Profile.AccountStyle;

    /// <summary>
    /// The bot's own display its programs open windows on (XMEETING.md §3); null for the machine's main
    /// display. Set with <see cref="OnDisplay"/>.
    /// </summary>
    public int? Display { get; private init; }

    /// <summary>This account with its commands, terminal and browser opening on its own display.</summary>
    public GuestAccount OnDisplay(int number)
    {
        if (number is < 1 or > 99)
        {
            Trace.TraceWarning($"GuestAccount.OnDisplay: refused display :{number} for {User}");
            throw new ArgumentOutOfRangeException(nameof(number));
        }
        return new GuestAccount(User, Uid, Home, IsDesktopUser, Profile, DisplayName, FullSudo) { Display = number, CarriesDesktopUser = CarriesDesktopUser };
    }

    /// <summary>
    /// True for the Website Tester where it has a Linux user of its own: it used to be the desktop's own user,
    /// so when its account is first made on a machine, the desktop user's files and browser logins are copied in.
    /// </summary>
    public bool CarriesDesktopUser { get; private init; }

    /// <summary>Written in the account's home once the desktop user's files and logins have been copied in.</summary>
    public string CarriedMarker => Home + "/.personabot/from-desktop-user";

    /// <summary>
    /// Run as root, once per machine: copies the desktop user's visible files, and its Firefox profile (cookies and
    /// saved logins), into this account, never over anything already there. The originals stay where they are.
    /// The marker is only written when the files copied in time, so a large home is finished on a later run.
    /// </summary>
    private string CarryDesktopUserScript()
    {
        Trace.WriteLine($"GuestAccount.CarryDesktopUserScript: {Profile.DesktopUser}'s files to {User}, if not already carried");
        var from = GuestShellText.Quote(Profile.DesktopHome);
        var fromBrowser = GuestShellText.Quote(Profile.DesktopBrowserProfile);
        var home = GuestShellText.Quote(Home);
        var browser = GuestShellText.Quote(BrowserProfile);
        var marker = GuestShellText.Quote(CarriedMarker);
        return $"if [ ! -e {marker} ] && [ -d {from} ]; then\n" +
               $"  mkdir -p {home}/.personabot\n" +
               $"  carried=1\n" +
               $"  for item in {from}/*; do [ -e \"$item\" ] || continue; timeout 120 cp -a -n \"$item\" {home}/ >/dev/null 2>&1 || carried=0; done\n" +
               $"  if [ -d {fromBrowser} ] && [ ! -e {browser}/cookies.sqlite ]; then mkdir -p {browser} && (timeout 60 cp -a -n {fromBrowser}/. {browser}/ >/dev/null 2>&1 || carried=0); rm -f {browser}/lock {browser}/.parentlock; fi\n" +
               $"  chown -R {User}:{User} {home} >/dev/null 2>&1 || true\n" +
               $"  if [ \"$carried\" = 1 ]; then date > {marker}; echo \"PERSONABOT_ACCOUNT_CARRIED: {Profile.DesktopUser}'s files and browser logins copied to {User}\"; fi\n" +
               "fi";
    }

    /// <summary>
    /// Run as root when the person moves this bot onto the main display (:0) or back: lets its user open windows
    /// there, or takes that away again. Best effort: the main display may already let every local user on.
    /// </summary>
    public string MainDisplayAccessScript(bool allow)
    {
        var rule = (allow ? "+" : "-") + "si:localuser:" + User;
        Trace.WriteLine($"GuestAccount.MainDisplayAccessScript: {User} {(allow ? "allowed on" : "taken off")} :0");
        return $"(DISPLAY=:0 xhost {rule} || su {Profile.DesktopUser} -c 'DISPLAY=:0 xhost {rule}') >/dev/null 2>&1; echo PERSONABOT_MAIN_DISPLAY {User} {(allow ? "on" : "off")}";
    }

    /// <summary>The X authority file for this account's own display: only its user may open windows there.</summary>
    public string DisplayAuthority(int number) => $"{Home}/.personabot-vps-{number}.Xauthority";

    /// <summary>The Firefox profile this account's browser uses.</summary>
    public string BrowserProfile => IsDesktopUser ? Profile.DesktopBrowserProfile : Home + "/.mozilla/firefox/persona";

    /// <summary>The account a persona runs as on this guest.</summary>
    public static GuestAccount For(Persona persona, GuestProfile profile)
    {
        ArgumentNullException.ThrowIfNull(persona);
        ArgumentNullException.ThrowIfNull(profile);
        Trace.WriteLine($"GuestAccount.For enter: persona {persona.Id} ({persona.Name}), user {persona.LinuxUser ?? "(tester)"}, style {profile.AccountStyle}");
        if (profile.AccountStyle == GuestAccountStyle.Android)
        {
            Trace.WriteLine($"GuestAccount.For exit: Android, root in /data/local/tmp/personabot/{persona.Id}");
            return new GuestAccount("root", 0, "/data/local/tmp/personabot/" + persona.Id, isDesktopUser: true, profile, persona.Name);
        }
        if (persona.LinuxUser == null)
        {
            Trace.WriteLine($"GuestAccount.For exit: Website Tester as {(profile.TesterUser is { } t ? t + " (own user)" : profile.DesktopUser + " (desktop user)")}");
            // The Website Tester: a Linux user of its own where the guest gives it one, else the desktop's own user.
            // It used to be the desktop's user, so its account carries that user's files and logins over, but only from
            // PersonaBot's own desktop user: never from the person's own account (PersonAccounts), whose logins are theirs.
            return profile.TesterUser is { } tester
                ? new GuestAccount(tester, GuestProfile.TesterUid, "/home/" + tester, isDesktopUser: false, profile, persona.Name, persona.FullSudo)
                    { CarriesDesktopUser = profile.DesktopUser == GuestNames.DesktopUser }
                : new GuestAccount(profile.DesktopUser, 0, profile.DesktopHome, isDesktopUser: true, profile, persona.Name, persona.FullSudo);
        }
        if (!System.Text.RegularExpressions.Regex.IsMatch(persona.LinuxUser, "^[a-z][a-z0-9]{1,23}$") || PersonaStore.ReservedUsers.Contains(persona.LinuxUser))
        {
            Trace.TraceWarning($"GuestAccount.For: refused: '{persona.LinuxUser}' is not a usable Linux user name (persona {persona.Id})");
            throw new ArgumentException($"'{persona.LinuxUser}' is not a usable Linux user name.");
        }
        if (persona.Uid is < 1000 or > 60000)
        {
            Trace.TraceWarning($"GuestAccount.For: refused: uid {persona.Uid} out of range (persona {persona.Id})");
            throw new ArgumentException($"{persona.Name}'s Linux user id {persona.Uid} is out of range.");
        }
        Trace.WriteLine($"GuestAccount.For exit: {persona.LinuxUser} ({persona.Uid}), full sudo={persona.FullSudo}");
        return new GuestAccount(persona.LinuxUser, persona.Uid, "/home/" + persona.LinuxUser, isDesktopUser: false, profile, persona.Name, persona.FullSudo);
    }

    /// <summary>
    /// A script run by the root guest shell that runs <paramref name="script"/> as this account, in its
    /// home folder. It stops with <see cref="MissingMarker"/> and exit code 97, before running anything,
    /// if the account or its helpers are missing, so <see cref="AccountShell"/> can set them up and retry.
    /// </summary>
    public string Wrap(string script)
    {
        ArgumentNullException.ThrowIfNull(script);
        var home = GuestShellText.Quote(Home);
        if (Style == GuestAccountStyle.Android)
            return $"mkdir -p {home} && cd {home} && HOME={home} /system/bin/sh -c {GuestShellText.Quote(script.Replace("\r\n", "\n"))}";
        // A bot's sudo rule that no longer matches its editor's "Full sudo" counts as missing too, so it is rewritten.
        var sudo = Style == GuestAccountStyle.Debian && !IsDesktopUser
            ? $" || {{ command -v sudo >/dev/null 2>&1 && test -d /etc/sudoers.d && ! grep -qxF {GuestShellText.Quote(SudoRule)} {SudoFile} 2>/dev/null; }}"
            : "";
        var check = $"if ! id -u {User} >/dev/null 2>&1 || ! test -d {home} || ! test -x {BrowserHelper} || ! test -x {BotTerminal.Helper}{sudo}; then echo {MissingMarker}; exit {MissingExitCode}; fi\n";
        var environment = $"export HOME={home} USER={User} LOGNAME={User} PATH={BinFolder}:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin " +
                          $"PERSONABOT_BROWSER_PROFILE={GuestShellText.Quote(BrowserProfile)} PERSONABOT_BROWSER_SHARED={(IsDesktopUser ? 1 : 0)} " +
                          (Display is { } own ? $"DISPLAY=:{own} XAUTHORITY={GuestShellText.Quote(DisplayAuthority(own))}\n" : "DISPLAY=\"${DISPLAY:-:0}\"\n") +
                          "cd \"$HOME\" || exit 1\n";
        if (Style == GuestAccountStyle.SharedRoot || User == "root")
        {
            // proot has one real user: the persona keeps its own home folder, but runs as root.
            return check + "(\n" + environment + script.Replace("\r\n", "\n") + "\n)";
        }
        return check + $"exec su -s /bin/sh {User} -c {GuestShellText.Quote(environment + script.Replace("\r\n", "\n"))}";
    }

    /// <summary>The root script that creates the account (idempotent) with its home, browser profile, helpers and display access.</summary>
    public string ProvisionScript()
    {
        Trace.WriteLine($"GuestAccount.ProvisionScript: {User} ({Uid}) on {Style}, desktop user={IsDesktopUser}");
        if (Style == GuestAccountStyle.Android) return $"mkdir -p {GuestShellText.Quote(Home)}; echo PERSONABOT_ACCOUNT_READY";
        var user = User;
        var uid = Uid.ToString(CultureInfo.InvariantCulture);
        var home = GuestShellText.Quote(Home);
        var lines = new List<string> { "set -e", $"mkdir -p {BinFolder}" };
        lines.Add($"cat > {BrowserHelper} <<'PERSONABOT_END'\n{BrowserHelperScript}PERSONABOT_END");
        lines.Add($"chmod 755 {BrowserHelper}");
        lines.Add($"cat > {BotTerminal.Helper} <<'PERSONABOT_END'\n{BotTerminal.HelperScript}PERSONABOT_END");
        lines.Add($"chmod 755 {BotTerminal.Helper}");
        if (Style is GuestAccountStyle.Debian or GuestAccountStyle.SharedRoot)
        {
            // The Bot terminal needs a terminal program; machines installed before it may have none. Best effort.
            lines.Add("command -v xterm >/dev/null 2>&1 || command -v aterm >/dev/null 2>&1 || (DEBIAN_FRONTEND=noninteractive timeout 120 apt-get install -y --no-install-recommends xterm >/dev/null 2>&1 || true)");
        }
        if (Style == GuestAccountStyle.Debian)
        {
            // Where the guest has sudo (the Mac's Debian image may not): apt only, or everything with "Full sudo".
            lines.Add($"if command -v sudo >/dev/null 2>&1 && test -d /etc/sudoers.d; then printf '%s\\n' {GuestShellText.Quote(SudoRule)} > {SudoFile}; chmod 440 {SudoFile}; fi");
        }
        if (!IsDesktopUser)
        {
            switch (Style)
            {
                case GuestAccountStyle.TinyCore:
                    // /etc/passwd is in RAM: recreate the user each boot with the same id, keeping the home on disk.
                    lines.Add($"id -u {user} >/dev/null 2>&1 || adduser -D -H -h {home} -s /bin/sh -G staff -u {uid} {user}");
                    lines.Add($"mkdir -p {home} && chown {user}:staff {home} && chmod 750 {home}");
                    // Packages: Tiny Core installs extensions as its own user, tc.
                    lines.Add($"rule='{user} ALL=(tc) NOPASSWD: /usr/bin/tce-load'; grep -qxF \"$rule\" /etc/sudoers || echo \"$rule\" >> /etc/sudoers");
                    lines.Add($"su tc -c 'DISPLAY=:0 xhost +si:localuser:{user}' >/dev/null 2>&1 || true");
                    break;
                case GuestAccountStyle.Debian:
                    // Its fixed id, unless something else already has it (the person's own account, made before the
                    // bots' ids were kept clear of, took 2003 on one machine): then the next free id from 2500.
                    lines.Add($"id -u {user} >/dev/null 2>&1 || {{ if getent passwd {uid} >/dev/null 2>&1 || getent group {uid} >/dev/null 2>&1; then " +
                              $"echo \"PersonaBot: user id {uid} is taken by $(getent passwd {uid} | cut -d: -f1); giving {user} another\" >&2; " +
                              $"useradd -m -d {home} -U -K UID_MIN=2500 -K UID_MAX=2999 -K GID_MIN=2500 -K GID_MAX=2999 -s /bin/bash {user}; " +
                              $"else useradd -m -d {home} -u {uid} -U -s /bin/bash {user}; fi; }}");
                    lines.Add($"mkdir -p {home} && chown {user}:{user} {home} && chmod 750 {home}");
                    lines.Add($"(DISPLAY=:0 xhost +si:localuser:{user} || su {Profile.DesktopUser} -c 'DISPLAY=:0 xhost +si:localuser:{user}') >/dev/null 2>&1 || true");
                    break;
                case GuestAccountStyle.SharedRoot:
                    lines.Add($"mkdir -p {home}");
                    break;
            }
            if (CarriesDesktopUser && Style == GuestAccountStyle.Debian) lines.Add(CarryDesktopUserScript());
            var profile = GuestShellText.Quote(BrowserProfile);
            lines.Add($"mkdir -p {profile}");
            lines.Add($"test -f {profile}/user.js || printf '%s\\n' {string.Join(" ", BrowserPreferences.Select(GuestShellText.Quote))} > {profile}/user.js");
            if (Style != GuestAccountStyle.SharedRoot)
            {
                var group = Style == GuestAccountStyle.TinyCore ? "staff" : user;
                lines.Add($"chown -R {user}:{group} {GuestShellText.Quote(Home + "/.mozilla")}");
            }
        }
        lines.Add("sync");
        lines.Add("echo PERSONABOT_ACCOUNT_READY");
        Trace.WriteLine($"GuestAccount.ProvisionScript exit: {User}, {lines.Count} steps, carries desktop user={CarriesDesktopUser}");
        return string.Join("\n", lines);
    }

    /// <summary>Opens the persona's own Firefox window (its own profile, so logins never mix between bots).</summary>
    private const string BrowserHelperScript = """
        #!/bin/sh
        # personabot-browser [url]: opens this persona's own Firefox window on the shared desktop.
        profile="${PERSONABOT_BROWSER_PROFILE:-$HOME/.mozilla/firefox/persona}"
        browser=$(command -v firefox-esr || command -v firefox || true)
        if [ -z "$browser" ]; then echo "Firefox is not installed in this computer."; exit 1; fi
        mkdir -p "$profile"
        if [ "${PERSONABOT_BROWSER_SHARED:-0}" = 1 ]; then
            DISPLAY="${DISPLAY:-:0}" nohup "$browser" --profile "$profile" "${1:-about:blank}" >"$HOME/firefox.log" 2>&1 &
        else
            DISPLAY="${DISPLAY:-:0}" nohup "$browser" --no-remote --profile "$profile" "${1:-about:blank}" >"$HOME/firefox.log" 2>&1 &
        fi
        echo "Opened your Firefox window (profile $profile). Inspect the screen to find it."

        """;

    private static readonly string[] BrowserPreferences =
    [
        "user_pref(\"browser.shell.checkDefaultBrowser\", false);",
        "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");",
        "user_pref(\"browser.aboutwelcome.enabled\", false);",
        "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);",
        "user_pref(\"toolkit.telemetry.reportingpolicy.firstRun\", false);",
        "user_pref(\"network.proxy.type\", 0);",
        "user_pref(\"browser.sessionstore.resume_from_crash\", false);"
    ];
}

/// <summary>
/// The guest's root shell, seen through one persona's account: every script runs as that user in its
/// home folder. A missing account is created (as root) and the script run again, once.
/// </summary>
public sealed class AccountShell(IGuestShell root, GuestAccount account) : IGuestShell
{
    public GuestAccount Account { get; } = account ?? throw new ArgumentNullException(nameof(account));

    private readonly IGuestShell _root = root ?? throw new ArgumentNullException(nameof(root));

    public async Task<string> ExecuteAsync(string script, CancellationToken token = default)
    {
        ArgumentNullException.ThrowIfNull(script);
        Trace.WriteLine($"AccountShell.ExecuteAsync enter: as {Account.User}{(Account.Display is { } display ? $" on :{display}" : "")}, {script.Length} chars");
        var watch = Stopwatch.StartNew();
        var output = await _root.ExecuteAsync(Account.Wrap(script), token);
        if (!IsMissing(output))
        {
            Trace.WriteLine($"AccountShell.ExecuteAsync exit: {Account.User} in {watch.ElapsedMilliseconds} ms, {output.Length} chars");
            return output;
        }
        Trace.WriteLine($"AccountShell.ExecuteAsync: setting up {Account.User}");
        var setup = await _root.ExecuteAsync(Account.ProvisionScript(), token);
        if (!setup.Contains("PERSONABOT_ACCOUNT_READY", StringComparison.Ordinal))
        {
            Trace.TraceError($"AccountShell.ExecuteAsync exit: setup of {Account.User} failed after {watch.ElapsedMilliseconds} ms: " + setup);
            throw new IOException($"Could not set up the Linux user {Account.User}: {Tail(setup)}");
        }
        Trace.WriteLine($"AccountShell.ExecuteAsync: {Account.User} set up, running the script again");
        output = await _root.ExecuteAsync(Account.Wrap(script), token);
        if (IsMissing(output))
        {
            Trace.TraceError($"AccountShell.ExecuteAsync: {Account.User} still missing after setup ({watch.ElapsedMilliseconds} ms)");
            throw new IOException($"The Linux user {Account.User} is still missing after setting it up.");
        }
        Trace.WriteLine($"AccountShell.ExecuteAsync exit: {Account.User} after setup in {watch.ElapsedMilliseconds} ms, {output.Length} chars");
        return output;
    }

    public Task SynchronizeAsync(CancellationToken token) => _root.SynchronizeAsync(token);

    private static bool IsMissing(string output) =>
        output.Contains(GuestAccount.MissingMarker, StringComparison.Ordinal)
        && output.TrimEnd().EndsWith("Exit code: " + GuestAccount.MissingExitCode, StringComparison.Ordinal);

    private static string Tail(string text)
    {
        var trimmed = text.Trim();
        return trimmed.Length <= 400 ? trimmed : "…" + trimmed[^400..];
    }
}
