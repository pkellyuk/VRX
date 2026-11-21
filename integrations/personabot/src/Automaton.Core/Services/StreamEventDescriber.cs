using Automaton.Core.Models;

namespace Automaton.Core.Services;

/// <summary>
/// Turns a conversation stream event into a short line a human can read at a glance:
/// "Running read…", "Finished bash", "Compacting context…".
///
/// Shared by the calendar scheduler's task cards and the Voice Director's worker
/// roster. Both are showing the same thing - what an agent is doing right now - and
/// they should not be free to word it differently.
/// </summary>
public static class StreamEventDescriber
{
    /// <summary>Fallback text when the event says nothing useful.</summary>
    public const string Working = StreamActivityText.Working;

    public static string Describe(RemoteConversationStreamEvent? streamEvent)
    {
        if (streamEvent == null)
        {
            return Working;
        }

        // The wording lives in StreamActivityText, which the Director app links too.
        return StreamActivityText.Describe(streamEvent.Kind, streamEvent.ToolName, streamEvent.RoleName, streamEvent.ErrorMessage);
    }
}
