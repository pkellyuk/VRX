using System;
using System.Collections.Generic;

namespace Automaton.Core.Models;

/// <summary>
/// How much a chat's model should write in its replies: terse, normal or verbose.
///
/// The level belongs to the chat, like its thinking effort. Normal is stored as null, so a chat
/// that has never touched the control sends exactly what it sent before this existed. The other
/// two add one instruction to each request (<see cref="InstructionFor"/>); nothing is written
/// into the conversation itself.
/// </summary>
public static class ResponseVerbosity
{
    public const string Terse = "terse";
    public const string Normal = "normal";
    public const string Verbose = "verbose";

    /// <summary>The levels in the order the dropdown shows them.</summary>
    public static readonly IReadOnlyList<string> Options = new[] { Terse, Normal, Verbose };

    /// <summary>
    /// The stored form of a level: <see cref="Terse"/>, <see cref="Verbose"/>, or null for
    /// normal, empty and anything unrecognised.
    /// </summary>
    public static string? Normalize(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var trimmed = value.Trim();
        if (string.Equals(trimmed, Terse, StringComparison.OrdinalIgnoreCase))
        {
            return Terse;
        }

        if (string.Equals(trimmed, Verbose, StringComparison.OrdinalIgnoreCase))
        {
            return Verbose;
        }

        return null;
    }

    /// <summary>The level to show for a stored value: never null.</summary>
    public static string Display(string? value)
    {
        return Normalize(value) ?? Normal;
    }

    /// <summary>The instruction a request carries for this level, or null for normal.</summary>
    public static string? InstructionFor(string? value)
    {
        var level = Normalize(value);
        if (level == Terse)
        {
            return "[Reply length: terse] Keep what you write to the user as short as it can be: the answer or the " +
                   "result first, in a few sentences, with no preamble, no recap of what you did and no closing " +
                   "summary. Leave out tables and long quotes unless asked. This is about your replies only - do " +
                   "not do less work or skip tool calls.";
        }

        if (level == Verbose)
        {
            return "[Reply length: verbose] Write your replies to the user in full: explain your reasoning, show the " +
                   "evidence you relied on, and spell out the steps and anything you were unsure of, rather than " +
                   "giving only the conclusion.";
        }

        return null;
    }
}
