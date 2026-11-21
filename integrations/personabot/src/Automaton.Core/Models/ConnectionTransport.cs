using System.Text.Json;
using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

// The transport level of the connection protocol: the message-type vocabulary, the
// envelope every message travels in, and the handshake/topology/error bodies.
//
// Split out of ConnectionProtocol.cs so a client can speak the protocol without
// dragging in the application-level bodies, which reference RemoteTreeSnapshot,
// Message and PermissionRequest and therefore most of the Core dependency graph.
// AndroidHelper links these sources directly (see AndroidHelper/AndroidHelper.csproj).
// Types are moved verbatim; nothing here changed on the way across.

public static class ConnectionMessageTypes
{
    public const string RegisterHello = "register_hello";
    public const string AuthChallenge = "auth_challenge";
    public const string AuthResponse = "auth_response";
    public const string RegisterAck = "register_ack";
    public const string TopologySnapshot = "topology_snapshot";
    public const string TreeSnapshot = "tree_snapshot";
    public const string ConversationUpdated = "conversation_updated";
    public const string SessionOpen = "session_open";
    public const string SessionOpened = "session_opened";
    public const string SessionInput = "session_input";
    public const string SessionEvent = "session_event";
    public const string SessionCancel = "session_cancel";
    public const string SessionClose = "session_close";
    public const string Ping = "ping";
    public const string Pong = "pong";
    public const string Error = "error";

    // Tree operation commands
    public const string ProjectAdd = "project_add";
    public const string ProjectRemove = "project_remove";
    public const string ProjectRename = "project_rename";
    public const string ProjectClone = "project_clone";
    public const string ThreadAdd = "thread_add";
    public const string ThreadRemove = "thread_remove";
    public const string ThreadRename = "thread_rename";
    public const string ThreadClone = "thread_clone";
    public const string ThreadSetAutomation = "thread_set_automation";
    public const string ConversationCompact = "conversation_compact";
    public const string ConversationSetPermissionMode = "conversation_set_permission_mode";
    public const string ConversationStop = "conversation_stop";

    // Goal operation commands
    public const string GoalAdd = "goal_add";
    public const string GoalUpdate = "goal_update";
    public const string GoalDelete = "goal_delete";
    public const string GoalMove = "goal_move";
    public const string GoalSnapshot = "goal_snapshot";

    // Screen viewing commands
    public const string ScreenViewRequest = "screen_view_request";
    public const string ScreenViewAccept = "screen_view_accept";
    public const string ScreenViewReject = "screen_view_reject";
    public const string ScreenViewStop = "screen_view_stop";
    public const string ScreenViewFrame = "screen_view_frame";
    public const string ScreenViewInput = "screen_view_input";

    // Provider share commands
    public const string ProviderShare = "provider_share";
    public const string ProviderShareResult = "provider_share_result";

    // Inbox alert broadcast commands
    public const string InboxAlert = "inbox_alert";

    // Request/Response style (no fire-and-forget)
    public const string GetTree = "get_tree";
    public const string TreeResponse = "tree_response";
    public const string GetConversation = "get_conversation";
    public const string ConversationResponse = "conversation_response";

    // Remote memory (remember tool) search — RPC against another device's local FTS index.
    public const string RememberSearch = "remember_search";
    public const string RememberSearchResponse = "remember_search_response";

    // Remote conversation fetch (get_conversation tool) — RPC for a single conversation's messages.
    public const string RememberGet = "remember_get";
    public const string RememberGetResponse = "remember_get_response";

    // Generic remote tool invocation — run any [IRemoteInvokable] tool on another device.
    public const string ToolInvoke = "tool_invoke";
    public const string ToolInvokeResponse = "tool_invoke_response";

    // Remote Director: a device with no speech stack of its own (a phone) drives the
    // Director on a device that has one. Dictation and sending are separate paths -
    // an utterance is only ever transcribed, and only director_text creates a turn.
    public const string DirectorAttach = "director_attach";
    public const string DirectorAttached = "director_attached";
    public const string DirectorDetach = "director_detach";
    public const string DirectorUtterance = "director_utterance";
    public const string DirectorTranscript = "director_transcript";
    public const string DirectorText = "director_text";
    public const string DirectorSay = "director_say";
    public const string DirectorEvent = "director_event";

    /// <summary>
    /// A still from the client's room camera, as a binary frame.
    ///
    /// One-way and unacknowledged, like a screen frame and unlike an utterance: the
    /// client is not waiting for anything, and a frame the host had no model to look at
    /// is a frame nobody needed to hear about. What comes back, if anything comes back at
    /// all, is a <see cref="DirectorEvent"/> of kind
    /// <see cref="DirectorEventKinds.Room"/>.
    /// </summary>
    public const string DirectorView = "director_view";

    /// <summary>
    /// Host to client: this person is called that.
    ///
    /// Names are learned in conversation - "the one in the chair is Paul" - and the
    /// Director is where that conversation happens, but the roster lives on the client
    /// because the client is where the faces are. So the name has to travel back.
    ///
    /// Costs no master change, because relaying is decided by the director_ prefix rather
    /// than by a list of known types.
    /// </summary>
    public const string DirectorNamePerson = "director_name_person";

    // Live voice on the client (LIVEVOICE.md §10i): the client streams its microphone up and
    // plays what streams down; the host runs the turn taking, speech-to-text, the Director and
    // speech. Bodies are in DirectorLiveProtocol.cs.
    public const string DirectorLiveStart = "director_live_start";
    public const string DirectorLiveStarted = "director_live_started";
    public const string DirectorLiveStop = "director_live_stop";
    public const string DirectorLiveAudio = "director_live_audio";
    public const string DirectorLiveSpeech = "director_live_speech";
    public const string DirectorLiveFlush = "director_live_flush";
    public const string DirectorLivePlayed = "director_live_played";
    public const string DirectorLiveState = "director_live_state";
    public const string DirectorLiveHeard = "director_live_heard";
}

/// <summary>
/// Shared knowledge about the director message family, so the master, the slave and the
/// host all agree on what counts as one.
/// </summary>
public static class DirectorProtocol
{
    /// <summary>Advertised by a device that can host a remote Director: model, speech in, speech out.</summary>
    public const string Capability = "director";

    /// <summary>Advertised by a device that drives someone else's Director but hosts none.</summary>
    public const string ClientCapability = "director_client";

    /// <summary>
    /// The prefix every director message type shares. Relaying is decided by this rather
    /// than by a list.
    /// </summary>
    public const string TypePrefix = "director_";

    /// <summary>
    /// True for every message in the director family, text or binary.
    ///
    /// Matched on the prefix, not against a list of the types known at compile time, and
    /// that distinction is worth more than it looks. The master relays director traffic by
    /// asking this question, and the master is a different machine on a different
    /// deployment cycle - so with a list, inventing a new director message meant every
    /// master had to be rebuilt and redeployed before a single one could cross, and until
    /// then a new binary type fell through to
    /// <c>HandleScreenViewFrameBinaryAsync</c> and was quietly misfiled as a screen frame.
    /// That is exactly what happened to <c>director_view</c>. On the prefix, a master only
    /// has to be new enough to know the rule, and never has to learn the types.
    ///
    /// Safe to widen: nothing outside this family is named <c>director_*</c>, and a type
    /// this returns true for that the far end does not handle is logged and dropped there
    /// rather than acted on.
    /// </summary>
    public static bool IsDirectorMessage(string? type)
    {
        if (string.IsNullOrWhiteSpace(type))
        {
            return false;
        }

        return type.StartsWith(TypePrefix, StringComparison.Ordinal);
    }
}

/// <summary>Audio carried by director_utterance and director_say.</summary>
public static class DirectorAudioFormats
{
    /// <summary>A RIFF/WAVE container holding 16-bit signed PCM.</summary>
    public const string Wav = "wav";

    public const int SampleRate = 16000;
    public const int Channels = 1;
    public const int BitsPerSample = 16;
}

/// <summary>Kinds carried by <see cref="DirectorEventBody"/>.</summary>
public static class DirectorEventKinds
{
    /// <summary>A one-line status from the Director itself.</summary>
    public const string Status = "status";

    /// <summary>A worker's live status line.</summary>
    public const string Worker = "worker";

    /// <summary>
    /// Something the Director noticed in the room the client's camera is pointed at.
    ///
    /// Deliberately an event kind rather than a message type of its own. A remark about
    /// the room is a line of text arriving unprompted, which is exactly what
    /// <c>director_event</c> already is - every client that can show one of those can
    /// show this without being taught anything new.
    /// </summary>
    public const string Room = "room";

    /// <summary>
    /// A short visual caption produced while the Director looks at a room frame.
    /// It is deliberately separate from <see cref="Room"/>: this is scene-only
    /// feedback, not a transcript entry or something to speak aloud.
    /// </summary>
    public const string SceneCaption = "scene_caption";
}

public sealed class DirectorAttachBody
{
    [JsonPropertyName("clientName")]
    public string ClientName { get; set; } = string.Empty;
}

public sealed class DirectorAttachedBody
{
    [JsonPropertyName("accepted")]
    public bool Accepted { get; set; } = true;

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;

    /// <summary>True when the host device's own voice loop is running.</summary>
    [JsonPropertyName("directorActive")]
    public bool DirectorActive { get; set; }

    /// <summary>False when the host can transcribe but cannot render speech to audio.</summary>
    [JsonPropertyName("canSpeak")]
    public bool CanSpeak { get; set; } = true;

    /// <summary>False when no speech-to-text service is reachable, so dictation will fail.</summary>
    [JsonPropertyName("canTranscribe")]
    public bool CanTranscribe { get; set; } = true;
}

public sealed class DirectorDetachBody
{
    [JsonPropertyName("reason")]
    public string Reason { get; set; } = string.Empty;
}

/// <summary>
/// The header of a director_utterance binary frame. The payload that follows it is the
/// audio itself.
/// </summary>
public sealed class DirectorUtteranceBody
{
    [JsonPropertyName("format")]
    public string Format { get; set; } = DirectorAudioFormats.Wav;

    [JsonPropertyName("sampleRate")]
    public int SampleRate { get; set; } = DirectorAudioFormats.SampleRate;

    [JsonPropertyName("channels")]
    public int Channels { get; set; } = DirectorAudioFormats.Channels;

    [JsonPropertyName("bits")]
    public int Bits { get; set; } = DirectorAudioFormats.BitsPerSample;

    /// <summary>Length of the clip, for logging and for spotting a truncated frame.</summary>
    [JsonPropertyName("durationMs")]
    public int DurationMs { get; set; }
}

/// <summary>
/// What a clip turned into. Carries replyTo of the originating utterance so a client
/// composing across several presses can keep them in the order they were spoken.
/// </summary>
public sealed class DirectorTranscriptBody
{
    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    /// <summary>Set when transcription failed; <see cref="Text"/> is then empty.</summary>
    [JsonPropertyName("error")]
    public string? Error { get; set; }
}

/// <summary>
/// The header of a director_view binary frame. The payload that follows it is the
/// encoded still.
///
/// The measurements the client already made travel with the frame rather than being
/// recomputed on the host. Only the client can know how the picture compares with the
/// one before it, because only the client has kept that one - the host is deliberately
/// not storing a frame history, and the whole point of the gate is that most frames
/// never leave the client at all.
/// </summary>
public sealed class DirectorViewBody
{
    /// <summary>IANA media type of the payload. Always an image type.</summary>
    [JsonPropertyName("mediaType")]
    public string MediaType { get; set; } = DirectorViewFormats.Jpeg;

    /// <summary>When the shutter fired, in UTC, round-trip formatted.</summary>
    [JsonPropertyName("capturedAt")]
    public DateTime CapturedAt { get; set; } = DateTime.UtcNow;

    /// <summary>
    /// Monotonic per-session counter. A gap in it says frames were dropped somewhere
    /// between the camera and the model, which is otherwise invisible.
    /// </summary>
    [JsonPropertyName("sequence")]
    public long Sequence { get; set; }

    /// <summary>One of the <see cref="DirectorViewReasons"/> constants.</summary>
    [JsonPropertyName("reason")]
    public string Reason { get; set; } = DirectorViewReasons.Interval;

    /// <summary>Mean movement since the last frame, in greyscale levels out of 255.</summary>
    [JsonPropertyName("motionScore")]
    public double MotionScore { get; set; }

    /// <summary>Overall lightening or darkening since the last frame, signed.</summary>
    [JsonPropertyName("brightnessDelta")]
    public double BrightnessDelta { get; set; }

    [JsonPropertyName("width")]
    public int Width { get; set; }

    [JsonPropertyName("height")]
    public int Height { get; set; }

    /// <summary>
    /// Who the client recognised in this frame, if it can recognise anyone at all.
    ///
    /// Empty carries no information either way - it means nobody was identified, which
    /// covers an empty room, a face too small to judge, and a client with no face models
    /// installed. <see cref="UnidentifiedFaces"/> is what separates those.
    /// </summary>
    [JsonPropertyName("people")]
    public List<DirectorViewPerson> People { get; set; } = [];

    /// <summary>
    /// Whether this client can recognise faces at all.
    ///
    /// Carried because an empty <see cref="People"/> list is ambiguous on its own: it
    /// covers an empty room, a face too far away to judge, recognition switched off, and
    /// models that were never installed. Without this the Director cannot tell "I do not
    /// see anyone I know" from "I am unable to know anyone", and it answers the second
    /// when the truth is the first.
    /// </summary>
    [JsonPropertyName("faceRecognitionAvailable")]
    public bool FaceRecognitionAvailable { get; set; }

    /// <summary>
    /// Faces seen but not identified - too small, or turned away.
    ///
    /// Worth carrying separately so the Director can say "someone is over by the window"
    /// without claiming to know who, which is the honest answer at that distance.
    /// </summary>
    [JsonPropertyName("unidentifiedFaces")]
    public int UnidentifiedFaces { get; set; }

    /// <summary>
    /// Whether a remark about this frame should be spoken as well as written.
    ///
    /// Carried per frame rather than agreed at attach time, so the switch on the client's
    /// settings screen takes effect on the next snapshot rather than at the next attach.
    /// </summary>
    [JsonPropertyName("speakComments")]
    public bool SpeakComments { get; set; } = true;
}

/// <summary>
/// Somebody the client's camera recognised in this frame.
///
/// An id and, once taught, a name - never an embedding and never a face. The numbers that
/// describe a face stay on the machine the camera is attached to; what crosses the wire
/// is the fact that "p1" is in the room.
/// </summary>
public sealed class DirectorViewPerson
{
    /// <summary>Stable per-install id, e.g. "p1".</summary>
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    /// <summary>What to call them, or empty when nobody has said yet.</summary>
    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    /// <summary>How sure the match was, 0 to 1. Below the client's threshold it is a new id.</summary>
    [JsonPropertyName("confidence")]
    public double Confidence { get; set; }

    /// <summary>True when this id was created by this frame rather than matched.</summary>
    [JsonPropertyName("isNew")]
    public bool IsNew { get; set; }
}

/// <summary>Host to client: give this id a name.</summary>
public sealed class DirectorNamePersonBody
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;
}

/// <summary>Encodings a director_view payload may be in.</summary>
public static class DirectorViewFormats
{
    public const string Jpeg = "image/jpeg";
}

/// <summary>Why a view was sent, which the host uses to steer what it asks the model.</summary>
public static class DirectorViewReasons
{
    /// <summary>The scheduled snapshot moved enough to be worth a look.</summary>
    public const string Interval = "interval";

    /// <summary>A person asked for it, so it is worth answering even if nothing moved.</summary>
    public const string Manual = "manual";

    /// <summary>
    /// Nothing tripped the gate for a long time, so this frame is sent to stop what the
    /// host remembers of the room drifting away from the room.
    /// </summary>
    public const string Reanchor = "reanchor";
}

/// <summary>The composed message. This, and only this, starts a Director turn.</summary>
public sealed class DirectorTextBody
{
    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    /// <summary>
    /// A chat on the host (its thread id): the message is for that chat's Director, scoped to the
    /// chat as the desktop composer's Director toggle does (XADVPS.md §0f). Empty: the main Director.
    /// </summary>
    [JsonPropertyName("chatId")]
    public string? ChatId { get; set; }
}

/// <summary>
/// The header of a director_say binary frame. The text is here as well as the audio, so
/// a client still shows the reply when playback fails or no audio was rendered.
/// </summary>
public sealed class DirectorSayBody
{
    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;

    [JsonPropertyName("hasAudio")]
    public bool HasAudio { get; set; }

    [JsonPropertyName("format")]
    public string Format { get; set; } = DirectorAudioFormats.Wav;

    [JsonPropertyName("sampleRate")]
    public int SampleRate { get; set; } = DirectorAudioFormats.SampleRate;

    [JsonPropertyName("channels")]
    public int Channels { get; set; } = DirectorAudioFormats.Channels;

    [JsonPropertyName("bits")]
    public int Bits { get; set; } = DirectorAudioFormats.BitsPerSample;
}

public sealed class DirectorEventBody
{
    [JsonPropertyName("kind")]
    public string Kind { get; set; } = DirectorEventKinds.Status;

    [JsonPropertyName("text")]
    public string Text { get; set; } = string.Empty;
}

public sealed class ConnectionEnvelope
{
    [JsonPropertyName("v")]
    public int Version { get; set; } = 1;

    [JsonPropertyName("type")]
    public string Type { get; set; } = string.Empty;

    [JsonPropertyName("id")]
    public string Id { get; set; } = Guid.NewGuid().ToString();

    [JsonPropertyName("sourceDeviceId")]
    public string SourceDeviceId { get; set; } = string.Empty;

    [JsonPropertyName("targetDeviceId")]
    public string? TargetDeviceId { get; set; }

    [JsonPropertyName("sessionId")]
    public string? SessionId { get; set; }

    [JsonPropertyName("replyTo")]
    public string? ReplyTo { get; set; }

    [JsonPropertyName("tsUtc")]
    public DateTime TsUtc { get; set; } = DateTime.UtcNow;

    [JsonPropertyName("body")]
    public JsonElement Body { get; set; }

    /// <summary>
    /// CRC32 checksum of the body JSON for integrity verification.
    /// Computed on send, verified on receive.
    /// </summary>
    [JsonPropertyName("crc")]
    public uint Checksum { get; set; }
}

public sealed class RegisterHelloBody
{
    [JsonPropertyName("deviceName")]
    public string DeviceName { get; set; } = string.Empty;

    [JsonPropertyName("platform")]
    public string Platform { get; set; } = string.Empty;

    [JsonPropertyName("role")]
    public string Role { get; set; } = string.Empty;

    /// <summary>The sender's build id (short commit and commit date, see BuildInfo). The wire name
    /// stays "appVersion" so devices on older builds still parse the hello.</summary>
    [JsonPropertyName("appVersion")]
    public string AppBuildId { get; set; } = string.Empty;

    [JsonPropertyName("capabilities")]
    public string[] Capabilities { get; set; } = [];
}

public sealed class AuthChallengeBody
{
    [JsonPropertyName("nonce")]
    public string Nonce { get; set; } = string.Empty;

    [JsonPropertyName("algorithm")]
    public string Algorithm { get; set; } = "HMACSHA256";
}

public sealed class AuthResponseBody
{
    [JsonPropertyName("timestampUtc")]
    public DateTime TimestampUtc { get; set; } = DateTime.UtcNow;

    [JsonPropertyName("signature")]
    public string Signature { get; set; } = string.Empty;
}

public sealed class RegisterAckBody
{
    [JsonPropertyName("masterDeviceId")]
    public string MasterDeviceId { get; set; } = string.Empty;

    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;
}

public sealed class TopologySnapshotBody
{
    [JsonPropertyName("masterDeviceId")]
    public string MasterDeviceId { get; set; } = string.Empty;

    [JsonPropertyName("devices")]
    public List<ConnectedDeviceInfo> Devices { get; set; } = [];
}

public sealed class ErrorBody
{
    [JsonPropertyName("message")]
    public string Message { get; set; } = string.Empty;
}
