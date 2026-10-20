# Test child: record only the launcher's relevant settings and literal arguments.
param([string]$OutputPath, [string]$First, [string]$Second)
[pscustomobject]@{
    Eye = $env:VRX_XG_EYE
    Shift = $env:VRX_XG_SHIFT
    Session = $env:VRX_XG_SESSION
    Layers = $env:VK_INSTANCE_LAYERS
    Paths = $env:VK_ADD_LAYER_PATH
    First = $First
    Second = $Second
} | ConvertTo-Json | Set-Content -LiteralPath $OutputPath -Encoding utf8
exit 7
