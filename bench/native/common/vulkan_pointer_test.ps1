#requires -Version 7.0
param([Parameter(Mandatory)][string]$Fixture,[Parameter(Mandatory)][string]$LayerDirectory,[switch]$Integer)
$ErrorActionPreference='Stop'
$root=Join-Path ([IO.Path]::GetTempPath()) ('VRX-VulkanPointer-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory $root
$log=Join-Path $root 'layer.log'
$start=[Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Fixture).Path)
$start.UseShellExecute=$false;$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
$start.Environment['VK_LAYER_PATH']=(Resolve-Path -LiteralPath $LayerDirectory).Path
$start.Environment['VRX_XG_LOG']=$log
if($Integer){$start.ArgumentList.Add('--integer')}
$child=[Diagnostics.Process]::Start($start)
try {
    $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
    if(-not $child.WaitForExit(30000)){$child.Kill();$child.WaitForExit();throw 'GPU pointer test timed out'}
    $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();Write-Output $output
    if($child.ExitCode -ne0){throw 'GPU pointer isolation failed'}
    $trace=Get-Content -LiteralPath $log -Raw
    if($trace -match 'STOPPED' -or $trace -notmatch 'mirror closed: 3 replayed submissions, [0-9]+ shader patches, 0 frames; live=1'){
        throw ('Pointer test did not prove three successful second-eye submissions: '+$trace)
    }
    Write-Output 'PASS: native buffer written once per dispatch while three second-eye submissions completed'
    Remove-Item -LiteralPath $log
    Remove-Item -LiteralPath $root
} finally { $child.Dispose() }
exit 0
