#requires -Version 7.0
param([Parameter(Mandatory)][string]$Fixture,[Parameter(Mandatory)][string]$Proxy,[Parameter(Mandatory)][string]$StatusTool)
$ErrorActionPreference='Stop'
$temporary=[IO.Path]::GetFullPath([IO.Path]::GetTempPath());$root=Join-Path $temporary ('VRX-native-launch-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root
$application=Join-Path $root 'native [fixture].exe';Copy-Item -LiteralPath $Fixture -Destination $application
$launcher=Join-Path $PSScriptRoot 'launch-native.ps1';$bridge=(Resolve-Path -LiteralPath $Proxy).Path;$status=(Resolve-Path -LiteralPath $StatusTool).Path
function Quote([string]$Text){return "'"+$Text.Replace("'","''")+"'"}
function StartLauncher([string]$Command){
    $start=[Diagnostics.ProcessStartInfo]::new('pwsh');$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($arg in @('-NoProfile','-EncodedCommand',[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Command)))){$start.ArgumentList.Add($arg)}
    return [Diagnostics.Process]::Start($start)
}
try {
    foreach($mode in @('mirror','observe','record','clone','queues','no-work','state','pipeline')) {
        $observe=$mode -eq 'observe';$recordOnly=$mode -in @('record','queues','no-work','state','pipeline');$cloneOnly=$mode -eq 'clone';$skipCommands=$mode -eq 'queues';$skipWork=$mode -eq 'no-work';$stateOnly=$mode -eq 'state';$recordCommands=$(if($mode -eq 'pipeline'){'SetPipelineState'}else{''})
        $session='launch-'+[Guid]::NewGuid().ToString('N');$stop=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,'Local\VRX.dx12.'+$session+'.fixture-stop')
        $command='& '+(Quote $launcher)+' -Executable '+(Quote $application)+' -Proxy '+(Quote $bridge)+' -Session '+(Quote $session)+' -GameArguments @('+(Quote ('--serve-gpu='+$session))+')'+$(if($observe){' -ObserveOnly'}elseif($recordOnly){' -RecordOnly'}elseif($cloneOnly){' -CloneOnly'}else{''})
        if($recordCommands){$command+=' -RecordCommands '+(Quote $recordCommands)}
        if($stateOnly){$command+=' -RecordStateOnly'}
        if($skipWork){$command+=' -SkipRecordingWork'}
        if($skipCommands){$command+=' -SkipCommandRecording'}
        $child=StartLauncher $command;$stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync();$game=$null
        try {
            $deadline=[Environment]::TickCount64+10000;$snapshot=$null
            do {
                $query=[Diagnostics.ProcessStartInfo]::new($status);$query.UseShellExecute=$false;$query.CreateNoWindow=$true;$query.RedirectStandardOutput=$true;$query.RedirectStandardError=$true;$query.ArgumentList.Add($session)
                $reader=[Diagnostics.Process]::Start($query)
                try {$json=$reader.StandardOutput.ReadToEnd();$null=$reader.StandardError.ReadToEnd();$reader.WaitForExit();if($reader.ExitCode -eq 0){$snapshot=$json|ConvertFrom-Json}}finally{$reader.Dispose()}
                if($child.HasExited){throw ('Launcher exited before rendering: '+$stderr.GetAwaiter().GetResult())}
                if($snapshot -and $snapshot.draws -gt 0 -and ($observe -or $recordOnly -or $cloneOnly -or $snapshot.publications -gt 0)){break}
                Start-Sleep -Milliseconds 20
            }while([Environment]::TickCount64 -lt $deadline)
            if(-not $snapshot -or -not $snapshot.draws -or $snapshot.mirroring -eq $observe -or $snapshot.hookFailures -or $snapshot.stops){throw 'Native child mode, API observation or replay status failed'}
            if(($observe -or $recordOnly -or $cloneOnly) -and ($snapshot.replays -or $snapshot.publications)){throw 'Diagnostic mode must forward native rendering without stereo replay'}
            $game=Get-Process -Id $snapshot.pid
            if(-not [string]::Equals($game.Path,$application,[StringComparison]::OrdinalIgnoreCase)){throw 'Status belongs to an unrelated application'}
            if(-not (Test-Path -LiteralPath (Join-Path $root 'd3d12.dll'))){throw 'Scoped bridge is missing while the fixture is running'}
            $null=$stop.Set();if(-not $child.WaitForExit(10000)){throw 'Launcher did not finish after fixture exit'}
            $output=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult()
            if($child.ExitCode -ne 0){throw ('Native launcher failed with exit '+$child.ExitCode+': '+$errors+'; output: '+$output)}
            if(Test-Path -LiteralPath (Join-Path $root 'd3d12.dll')){throw 'Launcher left its unchanged bridge installed'}
            $match=[regex]::Match($output,'run ([^\r\n]+)');if(-not $match.Success){throw 'Missing owned native run record'}
            $run=Get-Content -LiteralPath (Join-Path $match.Groups[1].Value 'run.json') -Raw|ConvertFrom-Json
            if($run.Executable -ne $application -or $run.Arguments.Count -ne 1 -or $run.Arguments[0] -ne ('--serve-gpu='+$session) -or $run.ObserveOnly -ne $observe -or $run.RecordOnly -ne $recordOnly -or $run.CloneOnly -ne $cloneOnly -or $run.SkipCommandRecording -ne $skipCommands -or $run.SkipRecordingWork -ne $skipWork -or $run.RecordStateOnly -ne $stateOnly -or $run.RecordCommands -ne $recordCommands){throw 'Owned run identity or literal arguments differ'}
        }finally{
            $null=$stop.Set();if($game -and -not $game.HasExited){$game.Kill();$game.WaitForExit()};if(-not $child.HasExited -and -not $child.WaitForExit(3000)){$child.Kill();$child.WaitForExit()}
            if($game){$game.Dispose()};$child.Dispose();$stop.Dispose()
        }
    }
    $invalid=StartLauncher ('& '+(Quote $launcher)+' -Executable '+(Quote $application)+' -Proxy '+(Quote $bridge)+' -ObserveOnly -CloneOnly -Preview')
    try{$null=$invalid.StandardOutput.ReadToEnd();$null=$invalid.StandardError.ReadToEnd();$invalid.WaitForExit();if($invalid.ExitCode -eq 0 -or (Test-Path -LiteralPath (Join-Path $root 'd3d12.dll'))){throw 'Conflicting diagnostics must be refused before installing files'}}finally{$invalid.Dispose()}
    $existing=Join-Path $root 'd3d12.dll';[IO.File]::WriteAllText($existing,'existing user DLL')
    $probe=StartLauncher ('& '+(Quote $launcher)+' -Executable '+(Quote $application)+' -Proxy '+(Quote $bridge)+' -Preview')
    try {$null=$probe.StandardOutput.ReadToEnd();$null=$probe.StandardError.ReadToEnd();$probe.WaitForExit();if($probe.ExitCode -eq 0 -or [IO.File]::ReadAllText($existing) -ne 'existing user DLL'){throw 'Existing game DLL must be preserved and launch refused'}}finally{$probe.Dispose()}
    Write-Output 'PASS: native launch, literal paths/arguments, replay/observation status, owned cleanup and existing-DLL preservation'
}finally{
    $resolved=[IO.Path]::GetFullPath($root)
    if(-not $resolved.StartsWith($temporary,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^VRX-native-launch-[0-9a-f]{32}$'){throw 'Refusing cleanup outside the owned fixture directory'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
