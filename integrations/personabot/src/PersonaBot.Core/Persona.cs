using System.Diagnostics;
using System.Text.RegularExpressions;

namespace PersonaBot;

/// <summary>
/// A persona: one of the bots living in the Linux computer (PERSONAS.md §3). Each persona is its own
/// Automaton project (its chats are that project's threads, so Automaton's per-project goals stay
/// separate later) and its own Linux user with a home folder. The model plays one persona at a time.
/// </summary>
public sealed class Persona
{
    /// <summary>Stable id and avatar-style slug, e.g. "researcher".</summary>
    public required string Id { get; init; }

    /// <summary>The Automaton project holding this persona's chats.</summary>
    public required string ProjectId { get; init; }

    public required string Name { get; set; }

    /// <summary>
    /// The persona's Linux user. Null for the Website Tester, whose user depends on the guest: its own
    /// ("tester") on Windows' Debian and Ubuntu machines, where it has a screen of its own; elsewhere the
    /// guest's desktop user (tc on Tiny Core, personabot on the Mac, root on Android). See <see cref="GuestAccount.For"/>.
    /// </summary>
    public string? LinuxUser { get; init; }

    /// <summary>The Linux user id, fixed so files keep their owner when Tiny Core recreates users at boot.</summary>
    public int Uid { get; init; }

    /// <summary>Avatar key: one of <see cref="PersonaStore.Avatars"/>.</summary>
    public string Avatar { get; set; } = "your-own";

    /// <summary>Accent colour (#RRGGBB) used by the switcher.</summary>
    public string Colour { get; set; } = "#F472B6";

    /// <summary>One line describing what this bot does.</summary>
    public string Tagline { get; set; } = "";

    /// <summary>The persona's own instructions, added to PersonaBot's shared rules.</summary>
    public string Instructions { get; set; } = "";

    public bool BuiltIn { get; init; }

    /// <summary>
    /// Whether the bot may run anything as root with sudo, without a password. Off by default: then sudo is only
    /// for installing packages with apt. Set in the bot's editor.
    /// </summary>
    public bool FullSudo { get; set; }

    /// <summary>The bot's own voice (a <see cref="VoiceCatalog"/> id), or null to use the voice in Settings → Voice.</summary>
    public string? Voice { get; set; }

    public int Position { get; set; }
}

/// <summary>The personas PersonaBot ships with (PRODUCT.md §1.2, PERSONAS.md §3).</summary>
public static class DefaultPersonas
{
    public const string WebsiteTesterId = "website-tester";

    public static IReadOnlyList<Persona> All { get; } =
    [
        new()
        {
            Id = WebsiteTesterId, ProjectId = ChatStore.DefaultProjectId, Name = "Website Tester", LinuxUser = null, Uid = 0,
            Avatar = "website-tester", Colour = "#F5B21B", BuiltIn = true, Position = 0,
            Tagline = "Tests websites and reports defects with evidence.",
            Instructions = """
                You are a focused website testing assistant. Work from the user's requested testing scope and
                ask for missing credentials or scope in chat. Distinguish a reproducible website defect from an
                agent or environment problem. Report expected behaviour, observed behaviour, reproduction
                steps and evidence. Never claim a test passed without observing it. Keep useful evidence and
                test scripts in your home folder.
                """
        },
        new()
        {
            Id = "researcher", ProjectId = "persona-researcher", Name = "Researcher", LinuxUser = "researcher", Uid = 2001,
            Avatar = "researcher", Colour = "#4F8CFF", BuiltIn = true, Position = 1,
            Tagline = "Browses, collects sources and summarises with links.",
            Instructions = """
                You research questions on the web. Search widely, open the sources themselves rather than
                trusting snippets, and prefer primary and recent sources. Summarise clearly with a link for
                every claim, say how sure you are, and separate facts from opinion. Point out when sources
                disagree. Keep your notes and collected sources in ~/research so later work can build on them.
                """
        },
        new()
        {
            Id = "admin-helper", ProjectId = "persona-admin-helper", Name = "Admin Helper", LinuxUser = "helper", Uid = 2002,
            Avatar = "admin-helper", Colour = "#22C98F", BuiltIn = true, Position = 2,
            Tagline = "Forms and routine web chores; asks before submitting.",
            Instructions = """
                You do routine web chores: filling in forms, checking accounts, downloading statements,
                booking and tidying. Always show the user what you are about to submit and wait for their
                agreement before pressing any submit, send, pay or confirm button. Ask the user to type
                passwords and one-time codes themselves. Keep a short log of what you did in ~/chores.md.
                """
        },
        new()
        {
            Id = "coder", ProjectId = "persona-coder", Name = "Coder", LinuxUser = "coder", Uid = 2003,
            Avatar = "coder", Colour = "#9B7BFA", BuiltIn = true, Position = 3,
            Tagline = "Writes and runs code in the Linux computer.",
            Instructions = """
                You write, run and debug code inside the Linux computer, mostly from the terminal with bash.
                Keep projects under ~/projects, use git for anything worth keeping, and run the code or its
                tests before saying it works. Explain what you changed and why in a few lines. Install tools
                only when needed and say what you installed.
                """
        },
        new()
        {
            Id = "accountant", ProjectId = "persona-accountant", Name = "Accountant", LinuxUser = "accountant", Uid = 2004,
            Avatar = "accountant", Colour = "#14B8A6", BuiltIn = true, Position = 4,
            Tagline = "Bookkeeping and sums, always showing the working.",
            Instructions = """
                You help with bookkeeping: reading statements and receipts, categorising, reconciling and
                building simple spreadsheets or CSV files in ~/books. Always show your working and check
                totals twice; flag every assumption and anything that does not reconcile. Never pay, transfer
                or move money, and ask before anything leaves this computer. You are not a licensed
                accountant or financial adviser: say so when asked for tax or investment advice.
                """
        }
    ];
}

/// <summary>
/// The personas, in PersonaBot's database (PersonaBotPersonas) beside Automaton's Projects table: each
/// persona row names its Automaton project. The built-in personas are seeded on first use; the
/// pre-persona chats (the "personabot" project) become the Website Tester's.
/// </summary>
public sealed class PersonaStore
{
    /// <summary>Avatar keys (PersonaBot.UI/Assets/avatars/{key}.png).</summary>
    public static IReadOnlyList<string> Avatars { get; } = ["website-tester", "researcher", "admin-helper", "coder", "accountant", "your-own"];

    /// <summary>Linux names a persona may never take: system accounts and the guests' desktop users.</summary>
    public static IReadOnlySet<string> ReservedUsers { get; } = new HashSet<string>(StringComparer.Ordinal)
    {
        "root", "tc", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail", "news", "uucp", "proxy",
        "www-data", "backup", "list", "irc", "gnats", "nobody", "nogroup", "staff", "admin", "sudo", "wheel", "users",
        "messagebus", "systemd-network", "systemd-resolve", "systemd-timesync", "sshd", "polkitd", "avahi", "ftp",
        "operator", "shutdown", "halt", "adm", "video", "render", "audio", "input", "tty", "disk", "kmem", "dialout",
        "personabot", "guest", "user", "home", "tester"
    };

    private const int FirstCustomUid = 2100;
    private static readonly Regex LinuxName = new("^[a-z][a-z0-9]{1,23}$", RegexOptions.CultureInvariant);
    private static readonly Regex HexColour = new("^#[0-9A-Fa-f]{6}$", RegexOptions.CultureInvariant);
    private readonly Automaton.Core.Services.ConfigDatabase _db;

    private readonly string _activePreference;

    public PersonaStore(string? machineId = null)
    {
        _activePreference = machineId == null ? "ActivePersona" : "ActivePersona:" + machineId;
        Trace.WriteLine("PersonaStore ctor enter");
        _db = PersonaBotDatabase.EnsureReady();
        _db.ExecuteInTransaction((_, _) =>
        {
            foreach (var persona in DefaultPersonas.All)
            {
                if (_db.ExecuteScalar<long>("SELECT COUNT(*) FROM PersonaBotPersonas WHERE Id = @Id", ChatStore.P("@Id", persona.Id)) > 0)
                {
                    continue;
                }
                Insert(persona);
                Trace.WriteLine($"PersonaStore: seeded {persona.Id}");
            }
            EnsureProjects();
        });
        Trace.WriteLine($"PersonaStore ctor exit: {All.Count} personas, active={ActiveId}");
    }

    /// <summary>All personas, built-ins first in their order, then the user's own by creation.</summary>
    public IReadOnlyList<Persona> All => _db.Query("SELECT * FROM PersonaBotPersonas ORDER BY Position, CreatedAt")
        .Select(Read)
        .ToList();

    /// <summary>The persona the app shows; the Website Tester until another is chosen.</summary>
    public string ActiveId
    {
        get
        {
            var id = _db.ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = @Key", ChatStore.P("@Key", _activePreference))
                ?? _db.ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = 'ActivePersona'");
            return id != null && All.Any(p => p.Id == id) ? id : DefaultPersonas.WebsiteTesterId;
        }
    }

    public Persona Active => Get(ActiveId);

    public Persona Get(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        if (All.FirstOrDefault(p => p.Id == id) is { } persona) return persona;
        Trace.TraceWarning($"PersonaStore.Get: there is no persona '{id}'");
        throw new ArgumentException($"There is no persona '{id}'.");
    }

    public void Activate(string id)
    {
        Get(id);
        Trace.WriteLine($"PersonaStore.Activate: {id}");
        _db.Execute(@"INSERT INTO PersonaBotPreferences (Key, Value) VALUES (@Key, @Id)
                ON CONFLICT(Key) DO UPDATE SET Value = excluded.Value", ChatStore.P("@Key", _activePreference), ChatStore.P("@Id", id));
    }

    /// <summary>A new persona of the user's own, with its own Automaton project and Linux user.</summary>
    public Persona Create(string name, string instructions, string avatar, string? tagline = null)
    {
        Trace.WriteLine($"PersonaStore.Create enter: '{name}'");
        var cleanName = CleanName(name);
        ValidateAvatar(avatar);
        var existing = All;
        if (existing.Any(p => string.Equals(p.Name, cleanName, StringComparison.OrdinalIgnoreCase)))
        {
            Trace.TraceWarning($"PersonaStore.Create: refused '{cleanName}': the name is taken");
            throw new ArgumentException($"There is already a persona called {cleanName}.");
        }
        var user = SuggestLinuxUser(cleanName, existing, PersonAccounts.Load()?.LinuxUser);
        Trace.WriteLine($"PersonaStore.Create: '{cleanName}' gets Linux user {user}, avatar {avatar}, instructions {instructions?.Length ?? 0} chars");
        var persona = new Persona
        {
            Id = user, ProjectId = "persona-" + user, Name = cleanName, LinuxUser = user,
            Uid = Math.Max(FirstCustomUid, existing.Select(p => p.Uid).DefaultIfEmpty(0).Max() + 1),
            Avatar = avatar, Colour = ColourFor(avatar), Tagline = (tagline ?? "").Trim(), Instructions = (instructions ?? "").Trim(),
            BuiltIn = false, Position = 100 + existing.Count
        };
        _db.ExecuteInTransaction((_, _) =>
        {
            Insert(persona);
            EnsureProject(persona);
        });
        Trace.WriteLine($"PersonaStore.Create exit: {persona.Id}, user {persona.LinuxUser} ({persona.Uid})");
        return persona;
    }

    /// <summary>Saves a persona's name, avatar, tagline and instructions. Its id, project and Linux user never change.</summary>
    public void Update(Persona persona)
    {
        ArgumentNullException.ThrowIfNull(persona);
        Trace.WriteLine($"PersonaStore.Update enter: {persona.Id}");
        var current = Get(persona.Id);
        var name = CleanName(persona.Name);
        ValidateAvatar(persona.Avatar);
        if (All.Any(p => p.Id != persona.Id && string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase)))
        {
            Trace.TraceWarning($"PersonaStore.Update: refused {persona.Id} as '{name}': the name is taken");
            throw new ArgumentException($"There is already a persona called {name}.");
        }
        var colour = HexColour.IsMatch(persona.Colour ?? "") ? persona.Colour! : current.Colour;
        Trace.WriteLine($"PersonaStore.Update: {persona.Id} as '{name}', avatar {persona.Avatar}, colour {colour}{(colour == persona.Colour ? "" : " (kept: given colour invalid)")}, full sudo {current.FullSudo} -> {persona.FullSudo}, voice {current.Voice ?? "default"} -> {persona.Voice ?? "default"}, instructions {persona.Instructions?.Length ?? 0} chars");
        _db.ExecuteInTransaction((_, _) =>
        {
            _db.Execute("UPDATE PersonaBotPersonas SET Name = @Name, Avatar = @Avatar, Colour = @Colour, Tagline = @Tagline, Instructions = @Instructions, FullSudo = @Sudo, Voice = @Voice WHERE Id = @Id",
                ChatStore.P("@Sudo", persona.FullSudo ? 1 : 0), ChatStore.P("@Voice", string.IsNullOrWhiteSpace(persona.Voice) ? null : persona.Voice), ChatStore.P("@Name", name), ChatStore.P("@Avatar", persona.Avatar), ChatStore.P("@Colour", colour),
                ChatStore.P("@Tagline", (persona.Tagline ?? "").Trim()), ChatStore.P("@Instructions", (persona.Instructions ?? "").Trim()), ChatStore.P("@Id", persona.Id));
            _db.Execute("UPDATE Projects SET Name = @Name, UpdatedAt = @Now WHERE Id = @Project",
                ChatStore.P("@Name", name), ChatStore.P("@Now", DateTime.UtcNow.ToString("O")), ChatStore.P("@Project", current.ProjectId));
        });
        Trace.WriteLine($"PersonaStore.Update exit: {persona.Id}");
    }

    /// <summary>
    /// Deletes one of the user's own personas and its chats. Built-in personas stay. The Linux user and
    /// its home folder in the guest are left alone (the guest may be off; files are never deleted silently).
    /// </summary>
    public void Delete(string id)
    {
        var persona = Get(id);
        if (persona.BuiltIn)
        {
            Trace.TraceWarning($"PersonaStore.Delete: refused {id}: built in");
            throw new InvalidOperationException($"{persona.Name} is built in and can't be deleted.");
        }
        Trace.WriteLine($"PersonaStore.Delete enter: {id}");
        var chats = new ChatStore(persona.ProjectId, projectName: persona.Name);
        Trace.WriteLine($"PersonaStore.Delete: removing {id}'s project {persona.ProjectId}, its chats and bookings");
        _db.ExecuteInTransaction((_, _) =>
        {
            foreach (var chat in chats.Chats)
            {
                chats.DeleteContent(chat.Id);
            }
            _db.Execute("DELETE FROM PersonaBotSession WHERE ProjectId = @Project", ChatStore.P("@Project", persona.ProjectId));
            // Its calendar bookings go with it (Automaton's occurrences cascade from CalendarTasks).
            _db.Execute("DELETE FROM CalendarTasks WHERE Id IN (SELECT TaskId FROM PersonaBotBookings WHERE PersonaId = @Id)", ChatStore.P("@Id", id));
            _db.Execute("DELETE FROM PersonaBotBookings WHERE PersonaId = @Id", ChatStore.P("@Id", id));
            _db.Execute("DELETE FROM Projects WHERE Id = @Project", ChatStore.P("@Project", persona.ProjectId));
            _db.Execute("DELETE FROM PersonaBotPersonas WHERE Id = @Id", ChatStore.P("@Id", id));
            if (_db.ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = 'ActivePersona'") == id)
            {
                _db.Execute("DELETE FROM PersonaBotPreferences WHERE Key = 'ActivePersona'");
            }
        });
        Trace.WriteLine($"PersonaStore.Delete exit: {id} deleted (its Linux user {persona.LinuxUser} stays in the guest)");
    }

    /// <summary>
    /// The Linux name for a new persona: its name in lower-case letters and digits, never a reserved or
    /// existing user, with a number added on a clash.
    /// </summary>
    /// <param name="person">The person's own Linux user (PersonAccounts), which no bot may take.</param>
    public static string SuggestLinuxUser(string name, IReadOnlyList<Persona> existing, string? person = null)
    {
        ArgumentNullException.ThrowIfNull(name);
        ArgumentNullException.ThrowIfNull(existing);
        Trace.WriteLine($"PersonaStore.SuggestLinuxUser enter: '{name}', {existing.Count} personas, person's user {person ?? "none"}");
        var letters = new string(name.ToLowerInvariant().Where(char.IsAsciiLetterOrDigit).ToArray());
        if (letters.Length < 2 || char.IsAsciiDigit(letters[0]))
        {
            // Linux names start with a letter.
            letters = "bot" + letters;
        }
        letters = letters[..Math.Min(letters.Length, 20)];
        var taken = existing.Select(p => p.LinuxUser).Concat(existing.Select(p => p.Id)).Append(person).Where(u => u != null).ToHashSet(StringComparer.Ordinal);
        var candidate = letters;
        for (var n = 2; ReservedUsers.Contains(candidate) || taken.Contains(candidate); n++)
        {
            candidate = letters + n.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }
        if (!LinuxName.IsMatch(candidate))
        {
            Trace.TraceWarning($"PersonaStore.SuggestLinuxUser: '{candidate}' from '{name}' is not a valid Linux name");
            throw new ArgumentException($"Could not make a Linux user name from '{name}'.");
        }
        Trace.WriteLine($"PersonaStore.SuggestLinuxUser exit: {candidate}{(candidate == letters ? "" : $" ({letters} was taken)")}");
        return candidate;
    }

    private static string CleanName(string name)
    {
        var clean = (name ?? "").Trim();
        if (clean.Length == 0)
        {
            Trace.TraceWarning("PersonaStore.CleanName: refused an empty name");
            throw new ArgumentException("Enter a name for the persona.");
        }
        if (clean.Length > 40)
        {
            Trace.TraceWarning($"PersonaStore.CleanName: refused a {clean.Length}-char name");
            throw new ArgumentException("Keep the persona's name to 40 characters.");
        }
        return clean;
    }

    private static void ValidateAvatar(string avatar)
    {
        if (!Avatars.Contains(avatar))
        {
            Trace.TraceWarning($"PersonaStore.ValidateAvatar: refused unknown avatar '{avatar}'");
            throw new ArgumentException($"Unknown avatar '{avatar}'.");
        }
    }

    private static string ColourFor(string avatar) =>
        DefaultPersonas.All.FirstOrDefault(p => p.Avatar == avatar)?.Colour ?? "#F472B6";

    private void Insert(Persona persona)
    {
        Trace.WriteLine($"PersonaStore.Insert: {persona.Id} (project {persona.ProjectId}, user {persona.LinuxUser ?? "desktop"} {persona.Uid}, built in={persona.BuiltIn})");
        _db.Execute(@"INSERT INTO PersonaBotPersonas (Id, ProjectId, Name, LinuxUser, Uid, Avatar, Colour, Tagline, Instructions, BuiltIn, Position, CreatedAt, FullSudo, Voice)
                VALUES (@Id, @Project, @Name, @User, @Uid, @Avatar, @Colour, @Tagline, @Instructions, @BuiltIn, @Position, @Now, @Sudo, @Voice)",
            ChatStore.P("@Sudo", persona.FullSudo ? 1 : 0), ChatStore.P("@Voice", string.IsNullOrWhiteSpace(persona.Voice) ? null : persona.Voice),
            ChatStore.P("@Id", persona.Id), ChatStore.P("@Project", persona.ProjectId), ChatStore.P("@Name", persona.Name),
            ChatStore.P("@User", persona.LinuxUser), ChatStore.P("@Uid", persona.Uid), ChatStore.P("@Avatar", persona.Avatar),
            ChatStore.P("@Colour", persona.Colour), ChatStore.P("@Tagline", persona.Tagline), ChatStore.P("@Instructions", persona.Instructions),
            ChatStore.P("@BuiltIn", persona.BuiltIn ? 1 : 0), ChatStore.P("@Position", persona.Position), ChatStore.P("@Now", DateTime.UtcNow.ToString("O")));
    }

    /// <summary>Every persona's Automaton project exists and carries its name; must run inside a transaction.</summary>
    private void EnsureProjects()
    {
        foreach (var persona in All)
        {
            EnsureProject(persona);
        }
    }

    private void EnsureProject(Persona persona)
    {
        var now = DateTime.UtcNow.ToString("O");
        var exists = _db.ExecuteScalar<long>("SELECT COUNT(*) FROM Projects WHERE Id = @Id", ChatStore.P("@Id", persona.ProjectId)) > 0;
        if (exists)
        {
            // The pre-persona project was called "PersonaBot"; it is the Website Tester's now.
            _db.Execute("UPDATE Projects SET Name = @Name WHERE Id = @Id AND Name <> @Name", ChatStore.P("@Name", persona.Name), ChatStore.P("@Id", persona.ProjectId));
            return;
        }
        // Left for ChatStore to create when the default project is new: it imports the old JSON chats then.
        if (persona.ProjectId == ChatStore.DefaultProjectId)
        {
            Trace.WriteLine($"PersonaStore.EnsureProject: {persona.ProjectId} is left for ChatStore to create");
            return;
        }
        _db.Execute("INSERT INTO Projects (Id, Name, Path, CreatedAt, UpdatedAt) VALUES (@Id, @Name, @Path, @Now, @Now)",
            ChatStore.P("@Id", persona.ProjectId), ChatStore.P("@Name", persona.Name), ChatStore.P("@Path", Settings.Root), ChatStore.P("@Now", now));
        Trace.WriteLine($"PersonaStore.EnsureProject: created {persona.ProjectId}");
    }

    private static Persona Read(Dictionary<string, object?> row) => new()
    {
        Id = (string)row["Id"]!, ProjectId = (string)row["ProjectId"]!, Name = (string)row["Name"]!, LinuxUser = row["LinuxUser"] as string,
        Uid = Convert.ToInt32(row["Uid"]), Avatar = (string)row["Avatar"]!, Colour = (string)row["Colour"]!, Tagline = (string)row["Tagline"]!,
        Instructions = (string)row["Instructions"]!, BuiltIn = Convert.ToInt64(row["BuiltIn"]) != 0, Position = Convert.ToInt32(row["Position"]),
        FullSudo = row.TryGetValue("FullSudo", out var sudo) && sudo != null && Convert.ToInt64(sudo) != 0,
        Voice = row.TryGetValue("Voice", out var voice) ? voice as string : null
    };
}
