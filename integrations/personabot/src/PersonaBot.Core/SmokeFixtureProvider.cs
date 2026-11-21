using Automaton.AI.Interfaces;
using Automaton.Core.Interfaces;
using Automaton.Core.Streaming;

namespace PersonaBot;

/// <summary>
/// A scripted provider for the smoke checks: first asks for a guest screenshot and a guest shell
/// command, then checks that both results (the image and the command's output) came back to it.
/// No paid model is called. Fails if any host tool was offered.
/// </summary>
public sealed class SmokeFixtureProvider(byte[]? expectedDisplay = null, string? expectedFocus = null, bool withoutTools = false,
    string? extraTool = null) : IAIProvider
{
    public const string ShellMarker = "PERSONABOT_MODEL_TOOL_LOOP_OK";
    private bool _called;

    public string Id => "offline-fixture";

    public Task<AIResponse> GenerateResponseAsync(List<Message> messages, Dictionary<string, JsonSchema> tools, string model,
        double? temperature = null, int? maxTokens = null, CancellationToken cancellationToken = default,
        Action<string>? textDeltaCallback = null, RawStreamRecorder? rawStreamRecorder = null)
    {
        ArgumentNullException.ThrowIfNull(messages);
        ArgumentNullException.ThrowIfNull(tools);
        System.Diagnostics.Trace.WriteLine($"SmokeFixtureProvider.GenerateResponseAsync enter: {messages.Count} messages, {tools.Count} tools, first call={!_called}");
        if (tools.ContainsKey("powershell") || tools.ContainsKey("remote"))
        {
            System.Diagnostics.Trace.TraceError("SmokeFixtureProvider.GenerateResponseAsync: host tools were offered");
            throw new IOException("Host tools were exposed.");
        }
        if (!_called)
        {
            if (expectedDisplay != null && !messages.Any(message => message.Content.Contains(expectedFocus ?? "") &&
                message.Attachments.Any(attachment => attachment.Base64Data == Convert.ToBase64String(expectedDisplay))))
                throw new IOException("The provider did not receive the full display and pointer focus together.");
            if (withoutTools)
            {
                if (tools.Count != 0) throw new IOException("Tools were offered while the guest was off.");
                return Task.FromResult(new AIResponse { Text = "Vrex replied while the guest was off." });
            }
            _called = true;
            System.Diagnostics.Trace.WriteLine("SmokeFixtureProvider.GenerateResponseAsync exit: asking for cu_grid and bash");
            var response = new AIResponse
            {
                ToolCalls =
                [
                    new() { Id = "fixture-screen", Tool = "cu_grid", Arguments = "{}" },
                    new() { Id = "fixture-shell", Tool = "bash", Arguments = "{\"command\":\"echo " + ShellMarker + "\"}" }
                ]
            };
            if (extraTool != null)
            {
                if (!tools.ContainsKey(extraTool)) throw new IOException("The provider did not get the Vrex tool alongside the guest tools.");
                response.ToolCalls.Add(new() { Id = "fixture-extra", Tool = extraTool, Arguments = "{}" });
            }
            return Task.FromResult(response);
        }
        if (!messages.Any(m => m.Attachments.Count > 0) || !messages.Any(m => m.Role == "tool" && m.Content.Contains(ShellMarker)))
        {
            System.Diagnostics.Trace.TraceError($"SmokeFixtureProvider.GenerateResponseAsync: image={messages.Any(m => m.Attachments.Count > 0)}, shell marker={messages.Any(m => m.Role == "tool" && m.Content.Contains(ShellMarker))}");
            throw new IOException("The provider did not receive the guest image and tool result.");
        }
        if (extraTool != null && !messages.Any(m => m.Role == "tool" && m.Content.Contains("VREX_EXTRA_TOOL_OK")))
            throw new IOException("A Vrex tool was routed to the guest instead of the Vrex worker.");
        System.Diagnostics.Trace.WriteLine("SmokeFixtureProvider.GenerateResponseAsync exit: passed");
        return Task.FromResult(new AIResponse { Text = "Offline provider fixture passed: guest tool calls and image feedback are working." });
    }
}
