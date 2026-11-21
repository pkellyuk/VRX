using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Automaton.Core.Models.Goals;

namespace Automaton.Core.Services.Goals;

public sealed partial class GoalTrackingService
{
    /// <summary>
    /// A requested change to a criterion's check command. A plain nullable string cannot say
    /// "leave it alone" and "clear it" apart, so the intent is explicit.
    /// </summary>
    public readonly struct CheckCommandChange
    {
        private CheckCommandChange(bool requested, string? value)
        {
            Requested = requested;
            Value = string.IsNullOrWhiteSpace(value) ? null : value.Trim();
        }

        public bool Requested { get; }
        public string? Value { get; }

        public static CheckCommandChange None => new(false, null);

        public static CheckCommandChange To(string? command)
        {
            return new CheckCommandChange(true, command);
        }
    }

    public GoalCriterion AddCriterion(GoalCaller caller, string goalId, GoalCriterionDraft draft)
    {
        if (draft == null)
        {
            throw new ArgumentNullException(nameof(draft));
        }

        if (string.IsNullOrWhiteSpace(draft.Description))
        {
            throw new GoalRuleException("A criterion needs a description.");
        }

        AutomatonDebug.Log($"GoalTrackingService.AddCriterion: Entry - goalId={goalId}, hasCommand={!string.IsNullOrWhiteSpace(draft.CheckCommand)}, caller={caller}");

        var defaultProof = DefaultProofFor(caller?.ProjectId);
        GoalCriterion? added = null;
        Mutate(caller!, goalId, null, context =>
        {
            added = InsertCriterion(context, draft, context.Goal.Criteria.Count + 1, defaultProof, context.Caller.Author);
            context.Goal.Criteria.Add(added);
            context.BumpWork();
            context.Journal(GoalJournalKinds.Scope, $"added criterion {added.ShortId} ({added.RequiredProof} proof): {Truncate(added.Description, 200)}");
            ReopenIfClosed(context, $"criterion {added.ShortId} added");
        });

        AutomatonDebug.Log($"GoalTrackingService.AddCriterion: Exit - goalId={goalId}, criterion={added!.ShortId}, proof={added.RequiredProof}");
        return added;
    }

    /// <summary>
    /// Changes a criterion. A model changing a criterion a human wrote - rewording it,
    /// changing its command, or asking for weaker proof - does not apply the change: it is
    /// parked for the human. Asking for stronger proof is always allowed.
    /// </summary>
    public GoalCriterionEditResult UpdateCriterion(
        GoalCaller caller,
        string goalId,
        string criterionRef,
        string? description,
        CheckCommandChange checkCommand,
        GoalProofKind? requiredProof)
    {
        if (string.IsNullOrWhiteSpace(criterionRef))
        {
            throw new ArgumentException("criterionRef is required", nameof(criterionRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.UpdateCriterion: Entry - goalId={goalId}, criterion={criterionRef}, caller={caller}");

        var result = new GoalCriterionEditResult();
        Mutate(caller, goalId, null, context => ChangeCriterion(context, ResolveCriterion(context.Goal, criterionRef), description, checkCommand, requiredProof, result));

        AutomatonDebug.Log($"GoalTrackingService.UpdateCriterion: Exit - goalId={goalId}, criterion={criterionRef}, parked={result.Parked}");
        return result;
    }

    /// <summary>
    /// The rule of a criterion change, inside the caller's transaction: applied, or parked
    /// for the human when a model would change what they wrote.
    /// </summary>
    private void ChangeCriterion(
        MutationContext context,
        GoalCriterion criterion,
        string? description,
        CheckCommandChange checkCommand,
        GoalProofKind? requiredProof,
        GoalCriterionEditResult result)
    {
        if (criterion == null)
        {
            throw new ArgumentNullException(nameof(criterion));
        }

        if (result == null)
        {
            throw new ArgumentNullException(nameof(result));
        }

        var rewording = description != null && !string.Equals(description.Trim(), criterion.Description, StringComparison.Ordinal);
        var recommanding = checkCommand.Requested && !string.Equals(checkCommand.Value, criterion.CheckCommand, StringComparison.Ordinal);
        var lowering = requiredProof.HasValue && requiredProof.Value < criterion.RequiredProof;
        var raising = requiredProof.HasValue && requiredProof.Value > criterion.RequiredProof;

        if (description != null && string.IsNullOrWhiteSpace(description))
        {
            throw new GoalRuleException("A criterion needs a description.");
        }

        var needsApproval = context.Caller.Author == GoalAuthor.Model &&
                            criterion.Author == GoalAuthor.Human &&
                            (rewording || recommanding || lowering);

        if (needsApproval)
        {
            var pending = new PendingScopeChange
            {
                Kind = "update",
                CriterionId = criterion.Id,
                CriterionShortId = criterion.ShortId,
                OldDescription = criterion.Description,
                NewDescription = rewording ? description!.Trim() : null,
                OldCheckCommand = criterion.CheckCommand,
                NewCheckCommand = recommanding ? checkCommand.Value : null,
                ChangesCheckCommand = recommanding,
                OldRequiredProof = criterion.RequiredProof,
                NewRequiredProof = lowering ? requiredProof : null,
                StateId = context.Caller.StateId,
                RequestedAt = context.Now
            };
            Park(context, pending);

            // Raising the bar needs nobody's permission, even alongside a parked change.
            if (raising)
            {
                ApplyCriterionChange(context, criterion, null, CheckCommandChange.None, requiredProof);
            }

            result.Parked = true;
            result.Criterion = criterion;
            result.Message = $"Recorded for human approval: {pending.Describe()}. {criterion.ShortId} is unchanged until they decide; carry on with other work.";
            return;
        }

        ApplyCriterionChange(context, criterion, rewording ? description : null, recommanding ? checkCommand : CheckCommandChange.None, requiredProof);
        result.Criterion = criterion;
        result.Message = $"{criterion.ShortId} updated.";
    }

    public GoalCriterionEditResult RemoveCriterion(GoalCaller caller, string goalId, string criterionRef)
    {
        if (string.IsNullOrWhiteSpace(criterionRef))
        {
            throw new ArgumentException("criterionRef is required", nameof(criterionRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.RemoveCriterion: Entry - goalId={goalId}, criterion={criterionRef}, caller={caller}");

        var result = new GoalCriterionEditResult();
        Mutate(caller, goalId, null, context =>
        {
            var criterion = ResolveCriterion(context.Goal, criterionRef);
            if (context.Caller.Author == GoalAuthor.Model && criterion.Author == GoalAuthor.Human)
            {
                var pending = new PendingScopeChange
                {
                    Kind = "remove",
                    CriterionId = criterion.Id,
                    CriterionShortId = criterion.ShortId,
                    OldDescription = criterion.Description,
                    StateId = context.Caller.StateId,
                    RequestedAt = context.Now
                };
                Park(context, pending);
                result.Parked = true;
                result.Criterion = criterion;
                result.Message = $"Recorded for human approval: {pending.Describe()}. It stays in place until they decide.";
                return;
            }

            DeleteCriterion(context, criterion);
            result.Message = $"{criterion.ShortId} removed.";
        });

        AutomatonDebug.Log($"GoalTrackingService.RemoveCriterion: Exit - goalId={goalId}, criterion={criterionRef}, parked={result.Parked}");
        return result;
    }

    /// <summary>
    /// Sets a criterion's status. How it became Met is decided by who is calling, never by an
    /// argument: a human's Met is human proof, a model's Met is only ever a model's say-so,
    /// and that is accepted only where the criterion asks for nothing stronger.
    /// </summary>
    public GoalCriterion SetCriterionStatus(GoalCaller caller, string goalId, string criterionRef, GoalCriterionStatus status, string? evidence)
    {
        if (string.IsNullOrWhiteSpace(criterionRef))
        {
            throw new ArgumentException("criterionRef is required", nameof(criterionRef));
        }

        AutomatonDebug.Log($"GoalTrackingService.SetCriterionStatus: Entry - goalId={goalId}, criterion={criterionRef}, status={status}, caller={caller}");

        GoalCriterion? changed = null;
        Mutate(caller, goalId, null, context =>
        {
            var criterion = ResolveCriterion(context.Goal, criterionRef);
            var previous = criterion.Status;
            GoalProofSource? metBy = null;

            if (status == GoalCriterionStatus.Met)
            {
                metBy = ProofOffered(context.Caller, criterion);
            }

            var wasSatisfied = criterion.IsSatisfied;
            criterion.Status = status;
            criterion.MetBy = metBy;
            if (!string.IsNullOrWhiteSpace(evidence))
            {
                criterion.Evidence = Cap(evidence, MaxEvidence);
            }

            if (status == GoalCriterionStatus.Met)
            {
                criterion.CheckedAt = context.Now;
            }

            WriteCriterion(context, criterion);

            if (previous != status)
            {
                context.Journal(GoalJournalKinds.StatusChange, $"{criterion.ShortId} {previous} -> {status}{(metBy.HasValue ? $" (by {metBy})" : string.Empty)}");
            }

            if (wasSatisfied && !criterion.IsSatisfied)
            {
                ReopenIfClosed(context, $"criterion {criterion.ShortId} is no longer met");
            }

            changed = criterion;
        });

        AutomatonDebug.Log($"GoalTrackingService.SetCriterionStatus: Exit - goalId={goalId}, criterion={changed!.ShortId}, status={changed.Status}, metBy={changed.MetBy?.ToString() ?? "-"}");
        return changed;
    }

    /// <summary>
    /// The legacy tools hand over a whole list of criterion descriptions. Criteria whose text
    /// is unchanged keep their status and evidence; new text is added; missing text is
    /// removed, under the same approval rule as any other removal.
    /// </summary>
    public List<string> ReplaceCriteriaByDescription(GoalCaller caller, string goalId, IEnumerable<string> descriptions)
    {
        if (descriptions == null)
        {
            throw new ArgumentNullException(nameof(descriptions));
        }

        return ReplaceCriteriaByDescription(caller, goalId, descriptions.Select(text => new GoalCriterionDraft { Description = text ?? string.Empty }));
    }

    /// <summary>
    /// As the list-of-strings form, with each criterion's proof (GOALSUPDATE.md §24.2). A new
    /// criterion is added with its command and proof. One whose text is unchanged keeps its
    /// status, and a command or proof that now differs is changed under the usual rule:
    /// applied to the caller's own, parked for approval on the human's. A draft that names no
    /// command or proof leaves the existing one alone.
    /// </summary>
    public List<string> ReplaceCriteriaByDescription(GoalCaller caller, string goalId, IEnumerable<GoalCriterionDraft> drafts)
    {
        if (drafts == null)
        {
            throw new ArgumentNullException(nameof(drafts));
        }

        var draftByText = new Dictionary<string, GoalCriterionDraft>(StringComparer.Ordinal);
        var wanted = new List<string>();
        foreach (var draft in drafts)
        {
            if (draft == null || string.IsNullOrWhiteSpace(draft.Description))
            {
                continue;
            }

            var text = draft.Description.Trim();
            if (draftByText.ContainsKey(text))
            {
                continue;
            }

            draftByText[text] = draft;
            wanted.Add(text);
        }

        AutomatonDebug.Log($"GoalTrackingService.ReplaceCriteriaByDescription: Entry - goalId={goalId}, criteria={wanted.Count}, withProof={draftByText.Values.Count(draft => draft.CheckCommand != null || draft.RequiredProof.HasValue)}, caller={caller}");

        var defaultProof = DefaultProofFor(caller?.ProjectId);
        var outcome = new List<string>();

        Mutate(caller!, goalId, null, context =>
        {
            var kept = 0;
            foreach (var existing in context.Goal.Criteria.ToList())
            {
                if (draftByText.TryGetValue(existing.Description, out var keptDraft))
                {
                    kept++;
                    var command = keptDraft.CheckCommand == null ? CheckCommandChange.None : CheckCommandChange.To(keptDraft.CheckCommand);
                    var commandDiffers = command.Requested && !string.Equals(command.Value, existing.CheckCommand, StringComparison.Ordinal);
                    var proofDiffers = keptDraft.RequiredProof.HasValue && keptDraft.RequiredProof.Value != existing.RequiredProof;
                    if (!commandDiffers && !proofDiffers)
                    {
                        continue;
                    }

                    var edit = new GoalCriterionEditResult();
                    ChangeCriterion(context, existing, null, command, keptDraft.RequiredProof, edit);
                    outcome.Add(edit.Parked ? $"change to {existing.ShortId} recorded for human approval" : $"{existing.ShortId} proof changed");
                    continue;
                }

                if (context.Caller.Author == GoalAuthor.Model && existing.Author == GoalAuthor.Human)
                {
                    var pending = new PendingScopeChange
                    {
                        Kind = "remove",
                        CriterionId = existing.Id,
                        CriterionShortId = existing.ShortId,
                        OldDescription = existing.Description,
                        StateId = context.Caller.StateId,
                        RequestedAt = context.Now
                    };
                    Park(context, pending);
                    outcome.Add($"removal of {existing.ShortId} recorded for human approval");
                    continue;
                }

                DeleteCriterion(context, existing);
                outcome.Add($"removed {existing.ShortId}");
            }

            foreach (var text in wanted)
            {
                if (context.Goal.Criteria.Any(criterion => string.Equals(criterion.Description, text, StringComparison.Ordinal)))
                {
                    continue;
                }

                var source = draftByText[text];
                var added = InsertCriterion(context, new GoalCriterionDraft { Description = text, CheckCommand = source.CheckCommand, RequiredProof = source.RequiredProof }, context.Goal.Criteria.Count + 1, defaultProof, context.Caller.Author);
                context.Goal.Criteria.Add(added);
                context.BumpWork();
                context.Journal(GoalJournalKinds.Scope, $"added criterion {added.ShortId} ({added.RequiredProof} proof): {Truncate(added.Description, 200)}");
                ReopenIfClosed(context, $"criterion {added.ShortId} added");
                outcome.Add($"added {added.ShortId}");
            }

            AutomatonDebug.Log($"GoalTrackingService.ReplaceCriteriaByDescription: kept {kept}, changes [{string.Join("; ", outcome)}]");
        });

        AutomatonDebug.Log($"GoalTrackingService.ReplaceCriteriaByDescription: Exit - goalId={goalId}, {outcome.Count} change(s)");
        return outcome;
    }

    // ── Scope approval ────────────────────────────────────────────────────

    public TrackedGoal ApproveScopeChange(GoalCaller caller, string goalId, string changeId)
    {
        return DecideScopeChange(caller, goalId, changeId, approve: true);
    }

    public TrackedGoal RejectScopeChange(GoalCaller caller, string goalId, string changeId)
    {
        return DecideScopeChange(caller, goalId, changeId, approve: false);
    }

    private TrackedGoal DecideScopeChange(GoalCaller caller, string goalId, string changeId, bool approve)
    {
        if (caller == null)
        {
            throw new ArgumentNullException(nameof(caller));
        }

        if (string.IsNullOrWhiteSpace(changeId))
        {
            throw new ArgumentException("changeId is required", nameof(changeId));
        }

        if (!caller.IsHuman)
        {
            AutomatonDebug.LogWarning($"GoalTrackingService.DecideScopeChange: refused - caller {caller} is not human");
            throw new GoalRuleException("Only the human can approve or reject a change to their criteria.");
        }

        AutomatonDebug.Log($"GoalTrackingService.DecideScopeChange: Entry - goalId={goalId}, changeId={changeId}, approve={approve}");

        Mutate(caller, goalId, null, context =>
        {
            var pending = context.Goal.PendingScopeChanges.FirstOrDefault(change => string.Equals(change.Id, changeId, StringComparison.OrdinalIgnoreCase));
            if (pending == null)
            {
                throw new GoalRuleException("That change is no longer waiting for approval.");
            }

            context.Goal.PendingScopeChanges.Remove(pending);
            WritePending(context);

            if (!approve)
            {
                context.Journal(GoalJournalKinds.Scope, $"human rejected: {pending.Describe()}");
                return;
            }

            var criterion = context.Goal.Criteria.FirstOrDefault(candidate => string.Equals(candidate.Id, pending.CriterionId, StringComparison.OrdinalIgnoreCase));
            if (criterion == null)
            {
                context.Journal(GoalJournalKinds.Scope, $"human approved a change to {pending.CriterionShortId}, which no longer exists");
                return;
            }

            context.Journal(GoalJournalKinds.Scope, $"human approved: {pending.Describe()}");
            if (string.Equals(pending.Kind, "remove", StringComparison.OrdinalIgnoreCase))
            {
                DeleteCriterion(context, criterion);
                return;
            }

            ApplyCriterionChange(
                context,
                criterion,
                pending.NewDescription,
                pending.ChangesCheckCommand ? CheckCommandChange.To(pending.NewCheckCommand) : CheckCommandChange.None,
                pending.NewRequiredProof);
        });

        var goal = GetGoal(caller, goalId);
        AutomatonDebug.Log($"GoalTrackingService.DecideScopeChange: Exit - goalId={goalId}, pending left={goal.PendingScopeChanges.Count}");
        return goal;
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    /// <summary>
    /// What kind of proof this caller's "Met" amounts to, or a refusal worded so that a model
    /// carries on instead of asking the human about it (GOALSUPDATE.md §5.6).
    /// </summary>
    private GoalProofSource ProofOffered(GoalCaller caller, GoalCriterion criterion)
    {
        if (caller.IsHuman)
        {
            return GoalProofSource.Human;
        }

        if (criterion.RequiredProof == GoalProofKind.Human)
        {
            AutomatonDebug.Log($"GoalTrackingService.ProofOffered: refused Met on {criterion.ShortId} - required=Human, offered=Model");
            throw new GoalRuleException($"{criterion.ShortId} is confirmed by the human at the final review. Attach your evidence with status unverified and carry on; do not ask them now.");
        }

        if (criterion.HasCheckCommand || criterion.RequiredProof == GoalProofKind.Check)
        {
            AutomatonDebug.Log($"GoalTrackingService.ProofOffered: refused Met on {criterion.ShortId} - required=Check, offered=Model, checksExecutable={ChecksExecutable}");
            // Only ever name a tool the calling chat really has.
            var route = "Its check command is confirmed at the final review. Attach your evidence with status unverified and carry on; do not ask the human now.";
            if (ChecksExecutable)
            {
                route = caller.AvailableToolIds != null && !caller.Lacks("goal_check")
                    ? "Run goal_check."
                    : "It becomes met when its check command is run and passes.";
            }

            throw new GoalRuleException($"{criterion.ShortId} is proven by its check command, not by saying so. {route}");
        }

        return GoalProofSource.Model;
    }

    private void ApplyCriterionChange(MutationContext context, GoalCriterion criterion, string? description, CheckCommandChange checkCommand, GoalProofKind? requiredProof)
    {
        var wasSatisfied = criterion.IsSatisfied;
        var meaningChanged = false;

        if (description != null)
        {
            var before = criterion.Description;
            criterion.Description = description.Trim();
            meaningChanged = true;
            context.Journal(GoalJournalKinds.Scope, $"reworded {criterion.ShortId}: \"{Truncate(before, 150)}\" -> \"{Truncate(criterion.Description, 150)}\"");
        }

        if (checkCommand.Requested)
        {
            var before = criterion.CheckCommand;
            criterion.CheckCommand = checkCommand.Value;
            criterion.CheckedWorkRevision = null;
            meaningChanged = true;
            context.Journal(GoalJournalKinds.Scope, $"{criterion.ShortId} check command: \"{before ?? "(none)"}\" -> \"{criterion.CheckCommand ?? "(none)"}\"");

            // A criterion that gains a command is proven by it from now on.
            if (criterion.HasCheckCommand && criterion.RequiredProof == GoalProofKind.Any && !requiredProof.HasValue)
            {
                criterion.RequiredProof = GoalProofKind.Check;
            }
        }

        if (requiredProof.HasValue && requiredProof.Value != criterion.RequiredProof)
        {
            context.Journal(GoalJournalKinds.Scope, $"{criterion.ShortId} required proof: {criterion.RequiredProof} -> {requiredProof.Value}");
            criterion.RequiredProof = requiredProof.Value;
        }

        if (criterion.RequiredProof == GoalProofKind.Check && !criterion.HasCheckCommand)
        {
            throw new GoalRuleException($"{criterion.ShortId} cannot require a check without a check command. Give it a command, or ask for human proof.");
        }

        if (meaningChanged)
        {
            // What was proven is not what is now asked for.
            criterion.Status = GoalCriterionStatus.Unverified;
            criterion.MetBy = null;
            context.BumpWork();
        }

        WriteCriterion(context, criterion);

        if (wasSatisfied && !criterion.IsSatisfied)
        {
            ReopenIfClosed(context, $"criterion {criterion.ShortId} changed");
        }
    }

    private void DeleteCriterion(MutationContext context, GoalCriterion criterion)
    {
        context.Execute("DELETE FROM GoalCriteria WHERE Id = @Id", P("@Id", criterion.Id));
        context.Goal.Criteria.Remove(criterion);

        for (var i = 0; i < context.Goal.Criteria.Count; i++)
        {
            var remaining = context.Goal.Criteria[i];
            if (remaining.Ordinal == i + 1)
            {
                continue;
            }

            remaining.Ordinal = i + 1;
            context.Execute("UPDATE GoalCriteria SET Ordinal = @Ordinal WHERE Id = @Id", P("@Ordinal", remaining.Ordinal), P("@Id", remaining.Id));
        }

        context.BumpWork();
        context.Journal(GoalJournalKinds.Scope, $"removed criterion {criterion.ShortId} ({criterion.Status}): {Truncate(criterion.Description, 200)}");
    }

    private void Park(MutationContext context, PendingScopeChange pending)
    {
        // One open proposal per criterion: a newer one replaces the older.
        context.Goal.PendingScopeChanges.RemoveAll(existing => string.Equals(existing.CriterionId, pending.CriterionId, StringComparison.OrdinalIgnoreCase));
        context.Goal.PendingScopeChanges.Add(pending);
        WritePending(context);
        context.Journal(GoalJournalKinds.Scope, $"proposed, awaiting human approval: {pending.Describe()}");
        AutomatonDebug.Log($"GoalTrackingService.Park: goal {context.Goal.Id} - {pending.Describe()} parked for approval");
    }

    private static void WritePending(MutationContext context)
    {
        var json = context.Goal.PendingScopeChanges.Count == 0 ? null : JsonSerializer.Serialize(context.Goal.PendingScopeChanges);
        context.Execute("UPDATE Goals SET PendingScopeChange = @Json WHERE Id = @Id", P("@Json", json), P("@Id", context.Goal.Id));
    }

    private static GoalCriterion ResolveCriterion(TrackedGoal goal, string criterionRef)
    {
        var wanted = criterionRef.Trim();
        var criterion = goal.Criteria.FirstOrDefault(candidate =>
            string.Equals(candidate.ShortId, wanted, StringComparison.OrdinalIgnoreCase) ||
            string.Equals(candidate.Id, wanted, StringComparison.OrdinalIgnoreCase));

        if (criterion == null)
        {
            throw new GoalRuleException($"There is no criterion {wanted} in this goal. Its criteria are: {(goal.Criteria.Count == 0 ? "(none)" : string.Join(", ", goal.Criteria.Select(candidate => candidate.ShortId)))}.");
        }

        return criterion;
    }

    private GoalCriterion InsertCriterion(MutationContext context, GoalCriterionDraft draft, int ordinal, GoalProofKind defaultProof, GoalAuthor author)
    {
        var number = ToLong(context.Query("SELECT NextCriterionNumber FROM Goals WHERE Id = @Id", P("@Id", context.Goal.Id))[0]["NextCriterionNumber"]);
        if (number < 1)
        {
            number = 1;
        }

        context.Execute("UPDATE Goals SET NextCriterionNumber = @Next WHERE Id = @Id", P("@Next", number + 1), P("@Id", context.Goal.Id));

        var command = string.IsNullOrWhiteSpace(draft.CheckCommand) ? null : draft.CheckCommand!.Trim();
        var proof = draft.RequiredProof ?? (command != null ? GoalProofKind.Check : defaultProof);
        if (proof == GoalProofKind.Check && command == null)
        {
            throw new GoalRuleException("A criterion cannot require a check without a check command. Give it a command, or ask for human proof.");
        }

        var criterion = new GoalCriterion
        {
            Id = Guid.NewGuid().ToString(),
            GoalId = context.Goal.Id,
            Ordinal = ordinal,
            ShortId = $"C{number}",
            Description = draft.Description.Trim(),
            CheckCommand = command,
            Status = GoalCriterionStatus.Unverified,
            RequiredProof = proof,
            Author = author == GoalAuthor.System ? GoalAuthor.Human : author
        };

        context.Execute(@"
            INSERT INTO GoalCriteria (Id, GoalId, Ordinal, ShortId, Description, CheckCommand, Status, RequiredProof, Author)
            VALUES (@Id, @GoalId, @Ordinal, @ShortId, @Description, @CheckCommand, @Status, @RequiredProof, @Author)",
            P("@Id", criterion.Id),
            P("@GoalId", criterion.GoalId),
            P("@Ordinal", criterion.Ordinal),
            P("@ShortId", criterion.ShortId),
            P("@Description", criterion.Description),
            P("@CheckCommand", criterion.CheckCommand),
            P("@Status", criterion.Status.ToString()),
            P("@RequiredProof", criterion.RequiredProof.ToString()),
            P("@Author", criterion.Author.ToString()));

        return criterion;
    }

    private static void WriteCriterion(MutationContext context, GoalCriterion criterion)
    {
        context.Execute(@"
            UPDATE GoalCriteria SET
                Description = @Description, CheckCommand = @CheckCommand, Status = @Status,
                RequiredProof = @RequiredProof, MetBy = @MetBy, Evidence = @Evidence,
                CheckedAt = @CheckedAt, CheckedWorkRevision = @CheckedWorkRevision
            WHERE Id = @Id",
            P("@Description", criterion.Description),
            P("@CheckCommand", criterion.CheckCommand),
            P("@Status", criterion.Status.ToString()),
            P("@RequiredProof", criterion.RequiredProof.ToString()),
            P("@MetBy", criterion.MetBy?.ToString()),
            P("@Evidence", criterion.Evidence),
            P("@CheckedAt", criterion.CheckedAt.HasValue ? Iso(criterion.CheckedAt.Value) : null),
            P("@CheckedWorkRevision", criterion.CheckedWorkRevision),
            P("@Id", criterion.Id));
    }
}
