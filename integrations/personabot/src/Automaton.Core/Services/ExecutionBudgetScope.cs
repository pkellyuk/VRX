using Automaton.AI.Interfaces;
using Automaton.Core.Interfaces;

namespace Automaton.Core.Services;

/// <summary>Opt-in accounting supplied by an owning host. Unscoped callers retain their normal behavior.</summary>
public interface IExecutionBudget
{
    CancellationToken Token { get; }
    IModelExecution BeginModel(List<Message> messages, Dictionary<string, JsonSchema> tools, int? maximumOutputTokens, CancellationToken token);
    void BeforeModelRequest();
    void Check();
}

public interface IModelExecution : IDisposable
{
    CancellationToken Token { get; }
    int? MaximumOutputTokens { get; }
    void AddDelta(string text);
    void Complete(AIResponse response);
}

public static class ExecutionBudgetScope
{
    private static readonly AsyncLocal<IExecutionBudget?> Active = new();
    public static IExecutionBudget? Current => Active.Value;
    public static IDisposable Enter(IExecutionBudget budget)
    { var previous = Active.Value; Active.Value = budget; return new Restore(previous); }
    private sealed class Restore(IExecutionBudget? previous) : IDisposable
    { public void Dispose() => Active.Value = previous; }
}

/// <summary>A provider that accounts each physical attempt using the current execution scope.</summary>
public interface IExecutionBudgetAwareProvider { }
