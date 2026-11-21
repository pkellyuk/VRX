using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

/// <summary>One milestone as the legacy tools and the remote protocol describe it.</summary>
public sealed class LegacyMilestone
{
    public string? Id { get; set; }
    public string? Description { get; set; }
    public List<string>? Criteria { get; set; }
    public List<string>? DependsOn { get; set; }

    /// <summary>The steps that make up this milestone, in order (GOALSUPDATE.md §24.2).</summary>
    public List<string>? Steps { get; set; }
}

public sealed partial class GoalTrackingService
{
    /// <summary>
    /// Milestones, then plan steps, as one ordered task list (GOALSUPDATE.md §24.2). A
    /// milestone's own steps come immediately before it and it waits on all of them; each step
    /// waits on whatever the milestone waits on. A milestone's criteria ride along as a bulleted
    /// tail. Dependencies are given by position in the list, so they hold however the tasks
    /// end up numbered. Plan steps are added at the end, unlinked.
    /// </summary>
    public static List<GoalTaskDraft> ToTaskDrafts(IEnumerable<LegacyMilestone>? milestones, IEnumerable<string>? plan)
    {
        var drafts = new List<GoalTaskDraft>();
        var kept = (milestones ?? Enumerable.Empty<LegacyMilestone>())
            .Where(milestone => milestone != null && !string.IsNullOrWhiteSpace(milestone.Description))
            .ToList();

        // Where each milestone's own task will sit, so a milestone may name one that comes later.
        var stepsByMilestone = kept.Select(StepsOf).ToList();
        var positionByMilestone = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var position = 0;
        for (var i = 0; i < kept.Count; i++)
        {
            position += stepsByMilestone[i].Count;
            if (!string.IsNullOrWhiteSpace(kept[i].Id) && !positionByMilestone.ContainsKey(kept[i].Id!.Trim()))
            {
                positionByMilestone[kept[i].Id!.Trim()] = position;
            }

            position++;
        }

        for (var i = 0; i < kept.Count; i++)
        {
            var milestone = kept[i];
            var steps = stepsByMilestone[i];
            var own = drafts.Count + steps.Count;

            var prerequisites = (milestone.DependsOn ?? new List<string>())
                .Where(id => !string.IsNullOrWhiteSpace(id) && positionByMilestone.ContainsKey(id.Trim()))
                .Select(id => positionByMilestone[id.Trim()])
                .Where(index => index != own)
                .Distinct()
                .ToList();

            var stepPositions = new List<int>();
            foreach (var step in steps)
            {
                stepPositions.Add(drafts.Count);
                drafts.Add(new GoalTaskDraft { Description = step, DependsOnDrafts = new List<int>(prerequisites) });
            }

            var description = milestone.Description!.Trim();
            var criteria = (milestone.Criteria ?? new List<string>()).Where(item => !string.IsNullOrWhiteSpace(item)).ToList();
            if (criteria.Count > 0)
            {
                description += "\n" + string.Join("\n", criteria.Select(item => $"- {item.Trim()}"));
            }

            drafts.Add(new GoalTaskDraft { Description = description, DependsOnDrafts = stepPositions.Concat(prerequisites).ToList() });
        }

        foreach (var step in plan ?? Enumerable.Empty<string>())
        {
            if (string.IsNullOrWhiteSpace(step))
            {
                continue;
            }

            drafts.Add(new GoalTaskDraft { Description = step.Trim() });
        }

        return drafts;
    }

    private static List<string> StepsOf(LegacyMilestone milestone)
    {
        return (milestone.Steps ?? new List<string>())
            .Where(step => !string.IsNullOrWhiteSpace(step))
            .Select(step => step.Trim())
            .ToList();
    }

    /// <summary>
    /// Gives each inserted task the dependencies its draft asked for, by task id and by
    /// position in the draft list. Runs once every task exists, so a draft may refer forwards.
    /// </summary>
    private static void ApplyDraftDependencies(MutationContext context, IReadOnlyList<GoalTaskDraft> drafts, IReadOnlyDictionary<int, GoalTask> insertedByDraft)
    {
        if (drafts == null)
        {
            throw new ArgumentNullException(nameof(drafts));
        }

        if (insertedByDraft == null)
        {
            throw new ArgumentNullException(nameof(insertedByDraft));
        }

        var applied = 0;
        foreach (var (index, target) in insertedByDraft.OrderBy(pair => pair.Key))
        {
            var draft = drafts[index];
            var wanted = new List<string>(draft.DependsOn ?? new List<string>());
            foreach (var position in draft.DependsOnDrafts ?? new List<int>())
            {
                if (position == index || !insertedByDraft.TryGetValue(position, out var prerequisite))
                {
                    AutomatonDebug.LogWarning($"GoalTrackingService.ApplyDraftDependencies: {target.ShortId} named draft position {position}, which is itself or was not inserted - ignored");
                    continue;
                }

                wanted.Add(prerequisite.ShortId);
            }

            var dependsOn = NormalizeRefs(wanted);
            if (dependsOn.Count == 0)
            {
                continue;
            }

            ValidateDependencies(context.Goal, target.ShortId, dependsOn);
            target.DependsOn = dependsOn;
            context.Execute("UPDATE GoalTasks SET DependsOn = @DependsOn WHERE Id = @Id",
                P("@DependsOn", JsonSerializer.Serialize(dependsOn)),
                P("@Id", target.Id));
            applied++;
        }

        AutomatonDebug.Log($"GoalTrackingService.ApplyDraftDependencies: goal {context.Goal.Id} - {applied} of {insertedByDraft.Count} task(s) given dependencies");
    }

    /// <summary>
    /// Applies a plan edited as a list of strings without losing progress: a step whose text
    /// is unchanged keeps its task and its status, new text becomes a new task at the end,
    /// and text that has gone removes its task. One transaction, so a refusal part-way
    /// leaves the plan exactly as it was.
    /// </summary>
    public TrackedGoal MergeTaskDescriptions(GoalCaller caller, string goalId, IEnumerable<string> descriptions)
    {
        if (descriptions == null)
        {
            throw new ArgumentNullException(nameof(descriptions));
        }

        var wanted = descriptions.Where(text => !string.IsNullOrWhiteSpace(text)).Select(text => text.Trim()).ToList();
        AutomatonDebug.Log($"GoalTrackingService.MergeTaskDescriptions: Entry - goalId={goalId}, wanted={wanted.Count}, caller={caller}");

        var added = 0;
        var removed = 0;
        Mutate(caller, goalId, null, context =>
        {
            // Match as a multiset, so two steps with the same text are two tasks.
            var unmatched = new List<string>(wanted);
            var toRemove = new List<GoalTask>();
            foreach (var task in context.Goal.Tasks)
            {
                if (unmatched.Remove(task.Description))
                {
                    continue;
                }

                toRemove.Add(task);
            }

            foreach (var task in toRemove)
            {
                RequireHolderOrHuman(context, task, "remove");
                var dependants = context.Goal.Tasks
                    .Where(other => !toRemove.Contains(other) && other.DependsOn.Contains(task.ShortId, StringComparer.OrdinalIgnoreCase))
                    .Select(other => other.ShortId)
                    .ToList();
                if (dependants.Count > 0)
                {
                    throw new GoalRuleException($"{task.ShortId} cannot be removed because {string.Join(", ", dependants)} depend on it. Remove that dependency first.");
                }
            }

            foreach (var task in toRemove)
            {
                context.Execute("DELETE FROM GoalTasks WHERE Id = @Id", P("@Id", task.Id));
                context.Goal.Tasks.Remove(task);
                context.Journal(GoalJournalKinds.Scope, $"removed {task.ShortId} ({task.Status}): {Truncate(task.Description, 200)}");
                removed++;
            }

            foreach (var text in unmatched)
            {
                var task = InsertTask(context, text, context.Goal.Tasks.Count + 1, new List<string>());
                context.Goal.Tasks.Add(task);
                context.Journal(GoalJournalKinds.Scope, $"added {task.ShortId}: {Truncate(task.Description, 200)}", task.Id);
                added++;
            }

            if (added == 0 && removed == 0)
            {
                return;
            }

            Renumber(context);
            context.BumpWork();
            if (added > 0)
            {
                ReopenIfClosed(context, $"{added} task(s) added");
            }
        });

        AutomatonDebug.Log($"GoalTrackingService.MergeTaskDescriptions: Exit - goalId={goalId}, added={added}, removed={removed}");
        return GetGoal(caller, goalId);
    }
}
