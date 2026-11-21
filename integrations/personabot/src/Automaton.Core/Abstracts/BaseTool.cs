using System;
using System.Text.Json;
using Automaton.Core.Interfaces;
using Automaton.Core.Services;

namespace Automaton.Core.Abstracts;

using Debug = AutomatonDebug;

/// <summary>
/// Base class for tool implementations with parameter validation.
/// </summary>
/// <typeparam name="TParameters">Type of the tool parameters.</typeparam>
public abstract class BaseTool<TParameters> : ITool where TParameters : class, new()
{
    public abstract string Id { get; }
    public abstract string Description { get; }
    public abstract JsonSchema Parameters { get; }

    /// <summary>
    /// Validates and parses tool arguments.
    /// </summary>
    /// <param name="args">JSON string arguments.</param>
    /// <returns>Parsed parameters.</returns>
    protected virtual TParameters ParseArguments(string args)
    {
        if (args == null)
        {
            throw new ArgumentNullException(nameof(args));
        }

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[BaseTool.ParseArguments] Entering with args length: {args.Length}");

        try
        {
            var options = new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            };
            var result = JsonSerializer.Deserialize<TParameters>(args, options);

            if (result == null)
            {
                AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[BaseTool.ParseArguments] Failed to parse tool arguments - null result");
                throw new ArgumentException("Failed to parse tool arguments");
            }

            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[BaseTool.ParseArguments] Successfully parsed arguments");
            return result;
        }
        catch (JsonException ex)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[BaseTool.ParseArguments] Invalid JSON arguments: {ex.Message}");
            throw new ArgumentException($"Invalid JSON arguments: {ex.Message}", ex);
        }
    }

    /// <summary>
    /// Executes the tool with validated parameters.
    /// </summary>
    /// <param name="args">Tool arguments as JSON string.</param>
    /// <param name="context">Execution context.</param>
    /// <returns>Tool execution result.</returns>
    public async Task<ToolResult> ExecuteAsync(string args, ToolContext context)
    {
        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[BaseTool.ExecuteAsync] Entering for tool: {Id}");

        try
        {
            // Validate arguments against schema (simplified validation)
            var parameters = ParseArguments(args);
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[BaseTool.ExecuteAsync] Executing internal for tool: {Id}");
            var result = await ExecuteInternalAsync(parameters, context);
            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[BaseTool.ExecuteAsync] Completed successfully for tool: {Id}");
            return result;
        }
        catch (Exception ex)
        {
            AutomatonDebug.Log(AutomatonDebug.DEBUG_ERROR, $"[BaseTool.ExecuteAsync] Error executing tool {Id}: {ex.Message}");
            throw;
        }
    }

    /// <summary>
    /// Tool implementation with typed parameters.
    /// </summary>
    /// <param name="parameters">Parsed and validated parameters.</param>
    /// <param name="context">Execution context.</param>
    /// <returns>Tool execution result.</returns>
    protected abstract Task<ToolResult> ExecuteInternalAsync(TParameters parameters, ToolContext context);

    /// <summary>
    /// Creates a JSON schema property.
    /// </summary>
    protected static JsonSchemaProperty Property(string type, string description)
    {
        return new JsonSchemaProperty
        {
            Type = type,
            Description = description
        };
    }

    /// <summary>
    /// Creates a JSON schema property with enum values.
    /// </summary>
    protected static JsonSchemaProperty Property(string type, string description, List<string> enumValues)
    {
        return new JsonSchemaProperty
        {
            Type = type,
            Description = description,
            Enum = enumValues
        };
    }

    /// <summary>
    /// Creates a JSON schema for object parameters.
    /// </summary>
    protected static JsonSchema CreateSchema(Dictionary<string, JsonSchemaProperty> properties, List<string>? required = null)
    {
        return new JsonSchema
        {
            Type = "object",
            Properties = properties,
            Required = required ?? new List<string>(),
            AdditionalProperties = false
        };
    }
}