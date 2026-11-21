using System.Text.Json;
using Automaton.Core.Abstracts;
using Automaton.Core.Interfaces;
using Automaton.Core.Models;
using Automaton.Core.Services;
using Automaton.Tools.Models;

namespace Automaton.Tools.Tools;

using Debug = AutomatonDebug;

/// <summary>
/// Tool for asking user questions, or broadcasting non-blocking alerts to the Inbox.
/// </summary>
public class QuestionTool : BaseTool<QuestionToolParameters>
{
    private readonly InboxService _inboxService;

    public override string Id => "question";

    public override string Description =>
        "Ask the user a question during execution, or broadcast a non-blocking alert to every connected device Inbox. " +
        "Default (broadcast=false) opens an interactive modal and BLOCKS until the user answers — only use this " +
        "when a human is actively present in this conversation. " +
        "Set broadcast=true to instead post a one-way alert to the local Inbox, broadcast it to peers, and return immediately, without waiting " +
        "for an answer. Use broadcast when no human is in the loop (e.g. a scheduled background check) or to surface " +
        "'something happened, check this' from any conversation. " +
        "IMPORTANT: in an autonomous/background context, before taking ANY write, update, exec, or otherwise " +
        "state-changing action, raise a broadcast alert describing what you found and the action you propose, then " +
        "stop and leave the action for the human. Reading/inspecting is fine without asking.";

    public override JsonSchema Parameters { get; }

    public QuestionTool(InboxService inboxService)
    {
        _inboxService = inboxService;

        Parameters = CreateSchema(
            new Dictionary<string, JsonSchemaProperty>
            {
                ["question"] = Property("string", "The complete question or alert message. Should be clear and specific."),
                ["options"] = new JsonSchemaProperty
                {
                    Type = "array",
                    Description = "The available choices for this question (2-4 options). Each option should be a distinct, mutually exclusive choice (unless multiSelect is enabled).",
                    Items = new JsonSchema
                    {
                        Type = "object",
                        Properties = new Dictionary<string, JsonSchemaProperty>
                        {
                            ["label"] = Property("string", "The display text for this option"),
                            ["description"] = Property("string", "A brief description of this option"),
                            ["markdown"] = Property("string", "Optional markdown content for this option")
                        },
                        Required = new List<string> { "label" }
                    }
                },
                ["multiSelect"] = Property("boolean", "Allow the user to select multiple options instead of just one. Default: false."),
                ["broadcast"] = Property("boolean", "When true, post a non-blocking alert to every connected device Inbox and return immediately instead of opening a blocking modal. Default: false."),
                ["title"] = Property("string", "Optional short title/heading for the inbox alert. Only used when broadcast=true.")
            },
            new List<string> { "question" }
        );
    }

    protected override async Task<ToolResult> ExecuteInternalAsync(QuestionToolParameters parameters, ToolContext context)
    {
        Debug.Log(Debug.DEBUG_INFO, $"[QuestionTool.ExecuteInternalAsync] Entering for question: {parameters.Question}, broadcast={parameters.Broadcast}");

        if (string.IsNullOrEmpty(parameters.Question))
        {
            Debug.Log(Debug.DEBUG_ERROR, $"[QuestionTool.ExecuteInternalAsync] question is required");
            throw new ArgumentException("question is required");
        }

        if (parameters.Broadcast)
        {
            return await BroadcastAsync(parameters, context);
        }

        Debug.Log(Debug.DEBUG_INFO, $"[QuestionTool.ExecuteInternalAsync] Showing question with {parameters.Options?.Count ?? 0} options");

        if (context.AskQuestionCallback == null)
        {
            Debug.Log(Debug.DEBUG_ERROR, "[QuestionTool.ExecuteInternalAsync] No interactive question handler is configured");
            throw new InvalidOperationException("Question tool requires an interactive question handler.");
        }

        var questionRequest = new QuestionRequest
        {
            Question = parameters.Question,
            MultiSelect = parameters.MultiSelect,
            Options = parameters.Options?
                .Select(option => new QuestionChoice
                {
                    Label = option.Label,
                    Description = option.Description,
                    Markdown = option.Markdown
                })
                .ToList() ?? new List<QuestionChoice>()
        };

        // A blocking question is invisible to anyone not looking at the surface it
        // opened on, and it holds the turn while they are not looking. Escalate to
        // the Inbox if it goes unanswered, which reaches every device rather than
        // the one that happened to be asked.
        using var answered = new CancellationTokenSource();
        var escalation = EscalateToInboxIfUnansweredAsync(parameters, context, answered.Token);

        QuestionResponse response;
        try
        {
            response = await context.AskQuestionCallback(questionRequest);
        }
        finally
        {
            answered.Cancel();
        }

        await escalation.ConfigureAwait(false);

        // Nobody answered in the time the waiter allows. Say so plainly rather
        // than handing back a blank that reads as "the user said nothing".
        if (response.TimedOut)
        {
            Debug.Log(Debug.DEBUG_WARNING, $"[QuestionTool.ExecuteInternalAsync] No answer within the timeout for: {parameters.Question}");
            return new ToolResult
            {
                Title = "User Question",
                Output = $"Question: {parameters.Question}"
                    + Environment.NewLine + Environment.NewLine
                    + "Nobody answered this within " + $"{QuestionRequest.AnswerTimeout.TotalMinutes:F0} minutes, so the turn stopped waiting. "
                    + "An alert was posted to the Inbox. Do not assume any particular answer - either continue with "
                    + "work that does not depend on it, or stop and explain what you need.",
                Metadata = new Dictionary<string, object>
                {
                    ["question"] = parameters.Question,
                    ["timedOut"] = true
                }
            };
        }

        var selectedOptions = response.SelectedOptions.Count > 0
            ? response.SelectedOptions
            : !string.IsNullOrWhiteSpace(response.Answer)
                ? new List<string> { response.Answer }
                : new List<string>();
        var answer = !string.IsNullOrWhiteSpace(response.Answer)
            ? response.Answer
            : string.Join(", ", selectedOptions);

        var output = new System.Text.StringBuilder();
        output.AppendLine($"Question: {parameters.Question}");
        output.AppendLine();

        if (parameters.Options != null && parameters.Options.Any())
        {
            output.AppendLine("Options:");
            for (int i = 0; i < parameters.Options.Count; i++)
            {
                var option = parameters.Options[i];
                output.AppendLine($"  {i + 1}. {option.Label}");
                if (!string.IsNullOrEmpty(option.Description))
                {
                    output.AppendLine($"     {option.Description}");
                }
            }

            if (parameters.MultiSelect)
            {
                output.AppendLine("(You can select multiple options, separated by commas)");
            }
        }
        else
        {
            output.AppendLine("(Please provide your answer)");
        }

        output.AppendLine($"User answered: {answer}");

        Debug.Log(Debug.DEBUG_INFO, $"[QuestionTool.ExecuteInternalAsync] Completed, answer: {answer}");

        return new ToolResult
        {
            Title = "User Question",
            Output = output.ToString(),
            Metadata = new Dictionary<string, object>
            {
                ["question"] = parameters.Question,
                ["answer"] = answer,
                ["selectedOptions"] = selectedOptions,
                ["options"] = parameters.Options?.Count ?? 0,
                ["multiSelect"] = parameters.MultiSelect
            }
        };
    }

    /// <summary>
    /// Posts an inbox alert if a blocking question is still unanswered after
    /// <see cref="QuestionRequest.EscalationDelay"/>, then returns.
    /// </summary>
    /// <param name="answered">
    /// Cancelled as soon as the question is answered, which is the normal case -
    /// the delay passes without an alert and this does nothing.
    /// </param>
    private async Task EscalateToInboxIfUnansweredAsync(
        QuestionToolParameters parameters,
        ToolContext context,
        CancellationToken answered)
    {
        if (parameters == null || context == null)
        {
            return;
        }

        try
        {
            await Task.Delay(QuestionRequest.EscalationDelay, answered).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Answered in time, which is what should normally happen.
            return;
        }

        Debug.Log(Debug.DEBUG_WARNING, $"[QuestionTool.EscalateToInboxIfUnansweredAsync] Unanswered after {QuestionRequest.EscalationDelay.TotalMinutes:F0} minutes, alerting the Inbox: {parameters.Question}");

        try
        {
            var item = new InboxItem
            {
                SourceSessionId = string.IsNullOrWhiteSpace(context.SessionId) ? null : context.SessionId,
                SourceTaskId = string.IsNullOrWhiteSpace(context.CalendarTaskId) ? null : context.CalendarTaskId,
                Title = "A chat is waiting on an answer",
                Message = parameters.Question,
                Options = parameters.Options?.Select(option => option.Label).ToList() ?? new List<string>()
            };

            await _inboxService.AddAsync(item).ConfigureAwait(false);

            Debug.Log(Debug.DEBUG_INFO, $"[QuestionTool.EscalateToInboxIfUnansweredAsync] Posted inbox alert id={item.Id}");
        }
        catch (Exception ex)
        {
            // The question is still live and still answerable; failing to raise the
            // alert must not take the turn down with it.
            Debug.Log(Debug.DEBUG_ERROR, $"[QuestionTool.EscalateToInboxIfUnansweredAsync] Failed to post inbox alert: {ex.Message}");
        }
    }

    private async Task<ToolResult> BroadcastAsync(QuestionToolParameters parameters, ToolContext context)
    {
        Debug.Log(Debug.DEBUG_INFO, $"[QuestionTool.BroadcastAsync] Entering - sessionId={context.SessionId}, task={context.CalendarTaskId}");

        var options = parameters.Options?.Select(option => option.Label).ToList() ?? new List<string>();

        var item = new InboxItem
        {
            SourceSessionId = string.IsNullOrWhiteSpace(context.SessionId) ? null : context.SessionId,
            SourceTaskId = string.IsNullOrWhiteSpace(context.CalendarTaskId) ? null : context.CalendarTaskId,
            Title = string.IsNullOrWhiteSpace(parameters.Title) ? null : parameters.Title,
            Message = parameters.Question,
            Options = options
        };

        await _inboxService.AddAsync(item);

        Debug.Log(Debug.DEBUG_INFO, $"[QuestionTool.BroadcastAsync] Posted inbox alert id={item.Id}");

        return new ToolResult
        {
            Title = "Inbox alert posted",
            Output = $"Posted a local inbox alert (id {item.Id}).",
            Metadata = new Dictionary<string, object>
            {
                ["inboxItemId"] = item.Id,
                ["broadcast"] = true,
                ["sourceSessionId"] = item.SourceSessionId ?? "",
                ["sourceTaskId"] = item.SourceTaskId ?? ""
            }
        };
    }
}
