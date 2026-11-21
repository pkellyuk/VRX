using PersonaBot;

namespace VRX.VrexHost;

internal sealed partial class VrexSession
{
    private static async Task<bool> PrepareUbuntuAccountAsync(IGuestShell shell, CancellationToken token)
    {
        var account = GuestAccount.For(Vrex, UbuntuProfile);
        var result = await shell.ExecuteAsync($$"""
            set -eu
            user={{UbuntuUser}}
            legacy={{GuestNames.DesktopUser}}
            if ! id "$user" >/dev/null 2>&1; then
                id "$legacy" >/dev/null
                systemctl stop personabot-desktop.service
                loginctl terminate-user "$legacy" >/dev/null 2>&1 || true
                usermod -l "$user" -d "/home/$user" -m "$legacy"
            fi
            # Keep the original account's numeric UID and group ownership.
            if [ "$(id -gn "$user")" = "$legacy" ]; then
                groupmod -n "$user" "$legacy"
            fi
            usermod -c 'V-Rex' "$user"
            if [ "$(hostname)" = "$legacy" ]; then
                # Existing cloud-init media still names the guest personabot.
                printf 'preserve_hostname: true\n' > /etc/cloud/cloud.cfg.d/99-vrex-hostname.cfg
                sed -i 's/^\(127\.0\.1\.1[[:space:]]\+\)personabot\([[:space:]]\|$\)/\1vrex\2/' /etc/hosts
                hostnamectl set-hostname "$user"
            fi
            if ! grep -Fq "runuser -u $user --" /usr/local/bin/personabot-session; then
                echo VRX_DESKTOP_ACCOUNT_CHANGED
            fi

            # Grant sudo only inside this persistent Ubuntu guest.
            rule=$(mktemp /etc/sudoers.d/.vrx-XXXXXX)
            trap 'rm -f "$rule"' EXIT
            printf '%s\n' '{{account.SudoRule}}' > "$rule"
            chmod 0440 "$rule"
            visudo -cf "$rule" >/dev/null
            mv -f "$rule" {{account.SudoFile}}
            runuser -u "$user" -- sudo -n true
            """, token);
        if (!result.TrimEnd().EndsWith("Exit code: 0", StringComparison.Ordinal))
            throw new IOException("Vrex's Ubuntu account could not be prepared: " + result.Trim());
        return result.Contains("VRX_DESKTOP_ACCOUNT_CHANGED", StringComparison.Ordinal);
    }
}
