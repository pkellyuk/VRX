using System.Text.Json.Serialization;

namespace PersonaBot.Voice;

// Shared by the helper and the app (PersonaBot.Desktop links this file): one JSON line per request or event.

/// <summary>
/// The app's side: one JSON line per request on the helper's stdin.
///   speak   — say <see cref="Text"/> in voice <see cref="Speaker"/> (Kokoro speaker id) with the
///             <see cref="Accent"/> ("gb" or "us") pronunciation, at <see cref="Speed"/>. It interrupts anything
///             being said. Events: level… then spoken. With <see cref="Listen"/> (and barge-in ready), the
///             microphone listens while it speaks: when the person talks over it, barge follows, speech stops
///             (spoken, Stopped), and what they say comes as heard with Id −Id (level… with −Id as they talk).
///   stop    — stop speaking now (spoken follows for the interrupted request, with Stopped).
///   listen  — open the microphone and listen until the person stops talking (<see cref="SilenceSeconds"/> of
///             quiet after speech), <see cref="MaxSeconds"/> pass, or finish/cancel arrive. Stops any speech first.
///             Events: listening, level…, optional partial text, then heard with the words.
///   finish  — stop listening now and turn what was heard into words.
///   cancel  — stop listening and forget it (heard follows with Cancelled).
///   devices — list the microphones (re-reading the devices when no audio is in use): devices follows, Text one per
///             line, "name, a tab, the full name to show", the system default first.
///   volume  — play speech at <see cref="Volume"/> (0 silent, 1 as made), from now on.
///   mic     — record from the microphone named <see cref="Text"/> (empty: the system default), from the next time it opens.
/// </summary>
public sealed record VoiceRequest(string Op, long Id = 0, string? Text = null, int Speaker = 0, string? Accent = null, float Speed = 1f,
    float MaxSeconds = 30f, float SilenceSeconds = 1.2f, bool Listen = false, float? Volume = null, float Pan = 0, float Rear = 0,
    float NoSpeechSeconds = 8, bool Partial = false);

/// <summary>
/// The helper's side: one JSON line per event on its stdout.
///   ready     — first line: what it can do (Speak: a voice model is loaded; Hear: a speech model and a microphone;
///               BargeIn: it can also listen while speaking, with the speech detector loaded).
///   barge     — the person has started talking over speak request Id; heard follows with Id −Id.
///   devices   — the microphones (Text: one name per line), for request Id.
///   level     — how loud (0–1) the speech being played or the microphone is, about 20 times a second.
///   listening — the microphone is open.
///   partial   — provisional words for this listening request, enabled with Partial.
///   spoken    — request Id has been said (or Stopped, or Error).
///   heard     — request Id's words (or Cancelled, or Error, or empty Text when nothing was said).
/// </summary>
public sealed record VoiceEvent(string Event, long Id = 0, bool Speak = false, bool Hear = false, float Level = 0, string? Text = null,
    bool Stopped = false, bool Cancelled = false, string? Error = null, double Ms = 0, bool BargeIn = false);

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingDefault)]
[JsonSerializable(typeof(VoiceRequest))]
[JsonSerializable(typeof(VoiceEvent))]
public sealed partial class VoiceJson : JsonSerializerContext;
