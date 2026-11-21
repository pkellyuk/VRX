using System.Text.Json.Serialization;

namespace Automaton.Core.Models;

public sealed class ConnectedDeviceInfo
{
    [JsonPropertyName("deviceId")]
    public string DeviceId { get; set; } = string.Empty;

    [JsonPropertyName("deviceName")]
    public string DeviceName { get; set; } = string.Empty;

    [JsonPropertyName("platform")]
    public string Platform { get; set; } = string.Empty;

    [JsonPropertyName("role")]
    public string Role { get; set; } = string.Empty;

    [JsonPropertyName("status")]
    public string Status { get; set; } = "online";

    [JsonPropertyName("capabilities")]
    public string[] Capabilities { get; set; } = [];
}
