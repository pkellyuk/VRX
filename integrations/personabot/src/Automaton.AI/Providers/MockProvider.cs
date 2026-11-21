using System;
using System.Text.Json;
using System.Threading;
using Automaton.AI.Interfaces;
using Automaton.Core.Interfaces;
using Automaton.Core.Streaming;
using Microsoft.Extensions.Logging;

namespace Automaton.AI.Providers;

/// <summary>
/// Mock AI provider for testing without real API calls.
/// </summary>
public class MockProvider : IAIProvider
{
    private readonly ILogger<MockProvider>? _logger;
    private readonly Random _random = new();

    public string Id => "mock";

    public MockProvider(ILogger<MockProvider>? logger = null)
    {
        _logger = logger;
    }

    public async Task<AIResponse> GenerateResponseAsync(
        List<Message> messages,
        Dictionary<string, JsonSchema> tools,
        string model,
        double? temperature = null,
        int? maxTokens = null,
        CancellationToken cancellationToken = default,
        Action<string>? textDeltaCallback = null,
        RawStreamRecorder? rawStreamRecorder = null)
    {
        _logger?.LogInformation("Mock provider generating response for {MessageCount} messages", messages.Count);

        // Simulate some processing time
        await Task.Delay(500, cancellationToken);

        // Get the last user message
        var lastUserMessage = messages.LastOrDefault(m => m.Role == "user");
        var goal = lastUserMessage?.Content ?? "No goal specified";

        // Simple mock logic based on goal
        var response = new AIResponse
        {
            Text = $"I understand your goal: {goal}. I'll help you with that.",
            FinishReason = "stop",
            Usage = new AIUsage
            {
                InputTokens = 100,
                OutputTokens = 50,
                TotalTokens = 150
            }
        };

        textDeltaCallback?.Invoke(response.Text);

        // If goal contains "read" or "file", suggest using read tool
        if (goal.Contains("read", StringComparison.OrdinalIgnoreCase) ||
            goal.Contains("file", StringComparison.OrdinalIgnoreCase))
        {
            if (tools.ContainsKey("read"))
            {
                response.ToolCalls.Add(new ToolCallRequest
                {
                    Id = $"call_{Guid.NewGuid():N}",
                    Tool = "read",
                    Arguments = JsonSerializer.Serialize(new
                    {
                        filePath = "./README.md"
                    })
                });
            }
        }
        // If goal contains "list" or "find", suggest using glob tool
        else if (goal.Contains("list", StringComparison.OrdinalIgnoreCase) ||
                 goal.Contains("find", StringComparison.OrdinalIgnoreCase))
        {
            if (tools.ContainsKey("glob"))
            {
                response.ToolCalls.Add(new ToolCallRequest
                {
                    Id = $"call_{Guid.NewGuid():N}",
                    Tool = "glob",
                    Arguments = JsonSerializer.Serialize(new
                    {
                        pattern = "**/*.cs"
                    })
                });
            }
        }
        // If goal contains "execute" or "command", suggest using shell tool
        else if (goal.Contains("execute", StringComparison.OrdinalIgnoreCase) ||
                 goal.Contains("command", StringComparison.OrdinalIgnoreCase) ||
                 goal.Contains("shell", StringComparison.OrdinalIgnoreCase))
        {
            if (tools.ContainsKey("shell"))
            {
                response.ToolCalls.Add(new ToolCallRequest
                {
                    Id = $"call_{Guid.NewGuid():N}",
                    Tool = "shell",
                    Arguments = JsonSerializer.Serialize(new
                    {
                        command = "ls -la",
                        description = "List files in current directory"
                    })
                });
            }
        }

        return response;
    }
}
