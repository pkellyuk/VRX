using System.Text.Json;
using System.Text.Json.Serialization;
using Automaton.Core.Abstracts;
using Automaton.Core.Interfaces;
using Automaton.Core.Services;
using Automaton.Core.Services.TaskLists;

namespace Automaton.Tools.Tools;

public class TaskListToolParameters
{
    [JsonPropertyName("items")]
    public List<TaskListItem>? Items { get; set; }

    [JsonPropertyName("clear")]
    public bool? Clear { get; set; }
}

/// <summary>
/// The chat's working task list: the model writes the whole list each time, marking each step
/// pending, in progress or completed, and the chat shows it pinned above the message box. The
/// model decides when it is no longer needed and clears it. See <see cref="ChatTaskListStore"/>.
/// </summary>
public class TaskListTool : BaseTool<TaskListToolParameters>
{
    private readonly Func<ChatTaskListStore> _store;

    public TaskListTool()
        : this(() => ChatTaskListStore.Shared)
    {
    }

    public TaskListTool(Func<ChatTaskListStore> store)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["items"] = new JsonSchemaProperty
                {
                    Type = "array",
                    Description = $"The whole list, in order - it replaces the previous one. At most {ChatTaskListStore.MaxItems} items.",
                    Items = new JsonSchema
                    {
                        Type = "object",
                        Properties = new Dictionary<string, JsonSchemaProperty>
                        {
                            ["text"] = Property("string", "The step, short and specific, e.g. \"Find the stock objects in PEROFFICE\"."),
                            ["status"] = new JsonSchemaProperty
                            {
                                Type = "string",
                                Description = "pending, in_progress or completed. Keep exactly one item in_progress while you work.",
                                Enum = TaskListItem.Statuses.ToList()
                            }
                        },
                        Required = new List<string> { "text", "status" },
                        AdditionalProperties = false
                    }
                },
                ["clear"] = new JsonSchemaProperty
                {
                    Type = "boolean",
                    Description = "true to remove the list - when the work is finished, or the user has moved on to something else."
                }
            },
            new List<string>());
    }

    public override string Id => TaskListToolInfo.ToolId;

    public override string Description =>
        "Keeps a short task list for the job in hand, shown to the user above the message box while you work. " +
        "Use it for any request that takes three or more distinct steps: write the steps first, then call it again " +
        "each time a step starts or finishes - mark one item in_progress, finished ones completed. Send the whole " +
        "list every time. When everything is done (or the user changes subject), leave the finished list up so the " +
        "user can see it, and clear it with clear=true when it is no longer useful - at the latest when you start " +
        "a new list. Not for one-step requests, and not a replacement for goals.";

    public override JsonSchema Parameters { get; }

    protected override Task<ToolResult> ExecuteInternalAsync(TaskListToolParameters parameters, ToolContext context)
    {
        if (parameters == null)
        {
            throw new ArgumentNullException(nameof(parameters));
        }

        if (context == null)
        {
            throw new ArgumentNullException(nameof(context));
        }

        AutomatonDebug.Log($"TaskListTool.ExecuteInternalAsync: Entry - session={context.SessionId}, items={parameters.Items?.Count ?? 0}, clear={parameters.Clear == true}");

        if (string.IsNullOrWhiteSpace(context.SessionId))
        {
            AutomatonDebug.LogError("TaskListTool.ExecuteInternalAsync: Exit - the chat has no session id");
            throw new InvalidOperationException("This chat has no session, so it cannot keep a task list.");
        }

        var store = _store();
        if (parameters.Clear == true || parameters.Items == null || parameters.Items.Count == 0)
        {
            store.Clear(context.SessionId);
            AutomatonDebug.Log("TaskListTool.ExecuteInternalAsync: Exit - list cleared");
            return Task.FromResult(Result("Task list cleared", "The task list is cleared and no longer shown.", Array.Empty<TaskListItem>()));
        }

        var tooMany = parameters.Items.Count > ChatTaskListStore.MaxItems;
        var saved = store.Set(context.SessionId, parameters.Items);
        if (saved.Count == 0)
        {
            AutomatonDebug.Log("TaskListTool.ExecuteInternalAsync: Exit - every item was blank, list cleared");
            return Task.FromResult(Result("Task list cleared", "Every item was blank, so the task list is cleared.", saved));
        }

        var notes = new List<string>();
        var inProgress = saved.Count(item => item.Status == TaskListItem.InProgress);
        if (inProgress > 1)
        {
            notes.Add($"{inProgress} items are in_progress; keep just the one you are working on.");
        }

        if (tooMany)
        {
            notes.Add($"Only the first {ChatTaskListStore.MaxItems} items were kept - a longer plan belongs in a goal.");
        }

        if (saved.All(item => item.Status == TaskListItem.Completed))
        {
            notes.Add("Everything is completed. Clear the list (clear=true) once the user no longer needs to see it.");
        }

        var output = $"Task list saved ({ChatTaskListStore.Describe(saved)}):\n{ChatTaskListStore.Render(saved)}" +
                     (notes.Count == 0 ? string.Empty : "\n\n" + string.Join("\n", notes));
        AutomatonDebug.Log($"TaskListTool.ExecuteInternalAsync: Exit - {ChatTaskListStore.Describe(saved)}");
        return Task.FromResult(Result($"Task list: {ChatTaskListStore.Describe(saved)}", output, saved));
    }

    private static ToolResult Result(string title, string output, IReadOnlyList<TaskListItem> items)
    {
        return new ToolResult
        {
            Title = title,
            Output = output,
            // The chat view redraws the pinned list from this, on this device or a remote one.
            Metadata = new Dictionary<string, object>
            {
                [TaskListToolInfo.ItemsMetadataKey] = JsonSerializer.Serialize(items)
            }
        };
    }
}
