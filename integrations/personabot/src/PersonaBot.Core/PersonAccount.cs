using System.Diagnostics;
using System.Text.RegularExpressions;

namespace PersonaBot;

/// <summary>
/// The person's own Linux account, made in every Debian and Ubuntu machine: their name, the user name, and the
/// password as Linux's one-way SHA-512 hash (the password itself is never kept). It has sudo without a password
/// prompt, and on machines installed after it was set, the desktop runs as this account.
/// </summary>
public sealed record PersonAccount(string Name, string LinuxUser, string PasswordHash)
{
    /// <summary>The account's home folder in a machine.</summary>
    public string Home => "/home/" + LinuxUser;
}

/// <summary>The person's account, kept in PersonaBot's preferences (PersonaBotPreferences).</summary>
public static class PersonAccounts
{
    private const string NameKey = "PersonName";
    private const string UserKey = "PersonLinuxUser";
    private const string HashKey = "PersonPasswordHash";
    private const string AskedKey = "PersonAccountAsked";

    /// <summary>The shortest password accepted.</summary>
    public const int MinimumPasswordLength = 8;

    private static readonly Regex LinuxName = new("^[a-z][a-z0-9]{1,23}$", RegexOptions.CultureInvariant);

    /// <summary>The account, or null until the person has made one.</summary>
    public static PersonAccount? Load()
    {
        var db = PersonaBotDatabase.EnsureReady();
        var name = Read(db, NameKey);
        var user = Read(db, UserKey);
        var hash = Read(db, HashKey);
        var account = name == null || user == null || !Sha512Crypt.IsHash(hash) ? null : new PersonAccount(name, user, hash!);
        Trace.WriteLine($"PersonAccounts.Load: {(account == null ? $"no account (name {(name == null ? "missing" : "set")}, user {user ?? "missing"}, hash {(hash == null ? "missing" : "set")})" : "user " + account.LinuxUser)}");
        return account;
    }

    /// <summary>
    /// Makes or changes the account. <paramref name="password"/> may be null to keep the current one when only the
    /// name changes. Throws <see cref="ArgumentException"/> with a message for the person when something is wrong.
    /// </summary>
    public static PersonAccount Save(string name, string linuxUser, string? password, IEnumerable<string?> personaUsers)
    {
        ArgumentNullException.ThrowIfNull(name);
        ArgumentNullException.ThrowIfNull(linuxUser);
        ArgumentNullException.ThrowIfNull(personaUsers);
        Trace.WriteLine($"PersonAccounts.Save enter: user '{linuxUser}', new password: {password != null}");
        var cleanName = name.Trim();
        if (cleanName.Length == 0)
        {
            Trace.TraceWarning("PersonAccounts.Save: refused: no name");
            throw new ArgumentException("Type your name.");
        }
        if (cleanName.Length > 60)
        {
            Trace.TraceWarning($"PersonAccounts.Save: refused: name of {cleanName.Length} chars");
            throw new ArgumentException("Your name is too long (60 characters at most).");
        }
        var user = linuxUser.Trim();
        if (ValidateUser(user, personaUsers) is { } problem)
        {
            Trace.TraceWarning($"PersonAccounts.Save: refused user '{user}': {problem}");
            throw new ArgumentException(problem);
        }
        var current = Load();
        string hash;
        if (password == null)
        {
            if (current == null)
            {
                Trace.TraceWarning("PersonAccounts.Save: refused: no password and no account yet");
                throw new ArgumentException("Choose a password.");
            }
            Trace.WriteLine("PersonAccounts.Save: keeping the current password");
            hash = current.PasswordHash;
        }
        else
        {
            if (ValidatePassword(password) is { } weak)
            {
                // The reason only; never the password.
                Trace.TraceWarning($"PersonAccounts.Save: refused the new password: {weak}");
                throw new ArgumentException(weak);
            }
            Trace.WriteLine("PersonAccounts.Save: hashing the new password");
            hash = Sha512Crypt.Hash(password);
        }
        var db = PersonaBotDatabase.EnsureReady();
        db.ExecuteInTransaction((_, _) =>
        {
            Write(db, NameKey, cleanName);
            Write(db, UserKey, user);
            Write(db, HashKey, hash);
            Write(db, AskedKey, "1");
        });
        Trace.WriteLine($"PersonAccounts.Save exit: {user}{(current == null ? " (new account)" : current.LinuxUser == user ? "" : $" (was {current.LinuxUser})")}");
        return new PersonAccount(cleanName, user, hash);
    }

    /// <summary>Why a Linux user name can't be the person's, or null when it can.</summary>
    public static string? ValidateUser(string user, IEnumerable<string?> personaUsers)
    {
        ArgumentNullException.ThrowIfNull(user);
        ArgumentNullException.ThrowIfNull(personaUsers);
        if (!LinuxName.IsMatch(user)) return "The Linux user name must be 2 to 24 small letters or digits, starting with a letter.";
        if (PersonaStore.ReservedUsers.Contains(user)) return $"'{user}' is kept for the system; choose another user name.";
        if (personaUsers.Any(u => string.Equals(u, user, StringComparison.Ordinal))) return $"A bot already uses the Linux user '{user}'; choose another.";
        return null;
    }

    /// <summary>Why a password can't be used, or null when it can.</summary>
    public static string? ValidatePassword(string password)
    {
        ArgumentNullException.ThrowIfNull(password);
        if (password.Length < MinimumPasswordLength) return $"Choose a password of at least {MinimumPasswordLength} characters.";
        if (password.Any(char.IsControl)) return "The password can't contain control characters.";
        return null;
    }

    /// <summary>A Linux user name from a person's name: the first name's letters and digits, lower case.</summary>
    public static string SuggestUser(string name, IEnumerable<string?> personaUsers)
    {
        ArgumentNullException.ThrowIfNull(name);
        ArgumentNullException.ThrowIfNull(personaUsers);
        var first = name.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
        var letters = new string(first.Normalize(System.Text.NormalizationForm.FormD).ToLowerInvariant().Where(char.IsAsciiLetterOrDigit).ToArray());
        if (letters.Length < 2 || char.IsAsciiDigit(letters[0])) letters = "me" + letters;
        letters = letters[..Math.Min(letters.Length, 20)];
        var taken = personaUsers.ToList();
        var candidate = letters;
        for (var n = 2; ValidateUser(candidate, taken) != null && n < 100; n++)
        {
            candidate = letters + n.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }
        return candidate;
    }

    /// <summary>Whether the person has been asked for their account (made it, or chose "Later").</summary>
    public static bool Asked => Read(PersonaBotDatabase.EnsureReady(), AskedKey) == "1" || Load() != null;

    /// <summary>Records that the person chose to make their account later (Settings still offers it).</summary>
    public static void MarkAsked()
    {
        Trace.WriteLine("PersonAccounts.MarkAsked");
        Write(PersonaBotDatabase.EnsureReady(), AskedKey, "1");
    }

    /// <summary>
    /// Run as root on a Debian or Ubuntu machine's console after every start, and when the account changes:
    /// makes the account if it is missing, sets its password hash, gives it sudo without a password, and
    /// puts it in the groups the desktop needs (video, render, audio). Idempotent; never restarts anything.
    /// </summary>
    /// <remarks>It runs inside the console's own shell, so it must never call exit.</remarks>
    public static string GuestScript(PersonAccount account)
    {
        ArgumentNullException.ThrowIfNull(account);
        if (!LinuxName.IsMatch(account.LinuxUser))
        {
            Trace.TraceWarning($"PersonAccounts.GuestScript: refused: '{account.LinuxUser}' isn't a valid Linux user name");
            throw new ArgumentException("The account's Linux user name isn't valid.", nameof(account));
        }
        if (!Sha512Crypt.IsHash(account.PasswordHash))
        {
            Trace.TraceWarning($"PersonAccounts.GuestScript: refused {account.LinuxUser}: the stored password hash isn't SHA-512 crypt");
            throw new ArgumentException("The account's password hash isn't a SHA-512 crypt hash.", nameof(account));
        }
        var user = account.LinuxUser;
        Trace.WriteLine($"PersonAccounts.GuestScript: account script for {user} (make, password hash, groups, sudo)");
        // The hash holds only ./0-9A-Za-z and $, so it is safe inside single quotes.
        return $$"""
            personabot_account_changed=""
            if ! id -u {{user}} >/dev/null 2>&1; then
                # An id below the bots' (2001 up), so it never takes one a bot is given.
                useradd -m -s /bin/bash -U -K UID_MIN=1000 -K UID_MAX=1999 -K GID_MIN=1000 -K GID_MAX=1999 -c {{GuestShellText.Quote(account.Name)}} {{user}} && personabot_account_changed="$personabot_account_changed made"
            fi
            if id -u {{user}} >/dev/null 2>&1; then
                if [ "$(getent shadow {{user}} | cut -d: -f2)" != '{{account.PasswordHash}}' ]; then
                    usermod -p '{{account.PasswordHash}}' {{user}} && personabot_account_changed="$personabot_account_changed password"
                fi
                usermod -c {{GuestShellText.Quote(account.Name)}} {{user}} >/dev/null 2>&1
                for personabot_group in sudo video render audio; do
                    if getent group "$personabot_group" >/dev/null 2>&1 && ! id -nG {{user}} | tr ' ' '\n' | grep -qx "$personabot_group"; then
                        usermod -a -G "$personabot_group" {{user}} && personabot_account_changed="$personabot_account_changed $personabot_group"
                    fi
                done
                if command -v visudo >/dev/null 2>&1 && test -d /etc/sudoers.d; then
                    printf '%s\n' '{{user}} ALL=(ALL:ALL) NOPASSWD: ALL' > /etc/sudoers.d/personabot-person.new
                    chmod 440 /etc/sudoers.d/personabot-person.new
                    if cmp -s /etc/sudoers.d/personabot-person.new /etc/sudoers.d/personabot-person; then
                        rm -f /etc/sudoers.d/personabot-person.new
                    elif visudo -cf /etc/sudoers.d/personabot-person.new >/dev/null 2>&1; then
                        mv -f /etc/sudoers.d/personabot-person.new /etc/sudoers.d/personabot-person && personabot_account_changed="$personabot_account_changed sudo"
                    else
                        rm -f /etc/sudoers.d/personabot-person.new
                    fi
                fi
                echo "PERSONABOT_PERSON_ACCOUNT {{user}} ready:${personabot_account_changed:- unchanged}"
            else
                echo "PERSONABOT_PERSON_ACCOUNT {{user}} could not be made"
            fi
            """;
    }

    private static string? Read(Automaton.Core.Services.ConfigDatabase db, string key) =>
        db.ExecuteScalar<string>("SELECT Value FROM PersonaBotPreferences WHERE Key = @Key", ChatStore.P("@Key", key));

    private static void Write(Automaton.Core.Services.ConfigDatabase db, string key, string value) =>
        db.Execute(@"INSERT INTO PersonaBotPreferences (Key, Value) VALUES (@Key, @Value)
                ON CONFLICT(Key) DO UPDATE SET Value = excluded.Value", ChatStore.P("@Key", key), ChatStore.P("@Value", value));
}
