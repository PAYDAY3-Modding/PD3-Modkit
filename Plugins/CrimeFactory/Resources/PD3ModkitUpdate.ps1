param(
    [string]$ConfigPath = '',
    [switch]$RepositoryWorker
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$script:AllowClose = $false
$script:State = 'WaitingForEditor'
$script:EditorClosedAt = $null
$script:ActiveProcess = $null
$script:ActiveStdOutTask = $null
$script:ActiveStdErrTask = $null
$script:UpdateCompleted = $false
$script:PullSucceeded = $false
$script:ReopenAt = $null
$script:BackupRoot = ''
$script:BackupContentPath = ''
$script:BackupCreated = $false
$script:ReadySignaled = $false
$script:RebuildPending = $false

$bootstrapLogPath = Join-Path $PSScriptRoot 'PD3ModkitUpdate.bootstrap.log'

function Write-BootstrapLog {
    param([string]$Message)

    try {
        $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
        "[$timestamp] $Message" | Out-File -FilePath $bootstrapLogPath -Append -Encoding utf8
    }
    catch {
    }
}

Write-BootstrapLog "Updater script entered. PowerShell PID: $PID"
Write-BootstrapLog "PowerShell version: $($PSVersionTable.PSVersion)"
Write-BootstrapLog "Script path: $PSCommandPath"
Write-BootstrapLog "Config path argument: $ConfigPath"

$EditorPid = 0
$RepositoryRoot = ''
$RemoteName = ''
$BranchName = ''
$LogPath = ''
$EditorExecutable = ''
$ProjectFile = ''
$ReadyFile = ''
$EngineDirectory = ''

try {
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        throw 'ConfigPath was not supplied.'
    }

    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
        throw "Updater config file does not exist: $ConfigPath"
    }

    $config = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json

    $EditorPid = [int]$config.EditorPid
    $RepositoryRoot = [string]$config.RepositoryRoot
    $RemoteName = [string]$config.RemoteName
    $BranchName = [string]$config.BranchName
    $LogPath = [string]$config.LogPath
    $EditorExecutable = [string]$config.EditorExecutable
    $ProjectFile = [string]$config.ProjectFile
    $ReadyFile = [string]$config.ReadyFile
    $EngineDirectory = [string]$config.EngineDirectory
    # Compatibility with launch configurations written by older plugin binaries.
    if ([string]::IsNullOrWhiteSpace($EngineDirectory)) {
        $EngineDirectory = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $EditorExecutable) '..\..'))
    }

    Write-BootstrapLog 'Updater config loaded successfully.'
}
catch {
    $fatalMessage = "FATAL config load error: $($_.Exception.Message)"
    Write-BootstrapLog $fatalMessage
    exit 8
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $PSScriptRoot 'PD3ModkitUpdate.log'
}

function Write-UpdateLog {
    param([string]$Message)

    try {
        $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
        "[$timestamp] $Message" | Out-File -FilePath $LogPath -Append -Encoding utf8
    }
    catch {
        Write-BootstrapLog "Logging failure: $($_.Exception.Message)"
    }
}

try {
    $logDirectory = Split-Path -Parent $LogPath
    if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    }

    if (-not (Test-Path -LiteralPath $LogPath)) { New-Item -ItemType File -Path $LogPath | Out-Null }

    if ($EditorPid -le 0) {
        throw 'EditorPid was not supplied or was invalid.'
    }

    if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
        throw 'RepositoryRoot was not supplied.'
    }

    if ([string]::IsNullOrWhiteSpace($RemoteName)) {
        throw 'RemoteName was not supplied.'
    }

    if ([string]::IsNullOrWhiteSpace($BranchName)) {
        throw 'BranchName was not supplied.'
    }

    if ([string]::IsNullOrWhiteSpace($EditorExecutable)) {
        throw 'EditorExecutable was not supplied.'
    }

    if ([string]::IsNullOrWhiteSpace($ProjectFile)) {
        throw 'ProjectFile was not supplied.'
    }

    if ([string]::IsNullOrWhiteSpace($ReadyFile)) {
        throw 'ReadyFile was not supplied.'
    }

    $projectDirectory = Split-Path -Parent $ProjectFile
    if ([string]::IsNullOrWhiteSpace($projectDirectory)) {
        throw "Could not determine the project directory from '$ProjectFile'."
    }

    $contentDirectory = Join-Path $projectDirectory 'Content'
    $localAppData = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        throw 'Windows did not return a LocalAppData directory.'
    }

    $backupBase = Join-Path $localAppData 'PAYDAY3-Modding\PD3-Modkit\UpdateBackups'

    Write-UpdateLog "Updater process started. PID: $PID"
    Write-UpdateLog "Repository: $RepositoryRoot"
    Write-UpdateLog "Remote: $RemoteName"
    Write-UpdateLog "Branch: $BranchName"
    Write-UpdateLog "Project: $ProjectFile"
    Write-UpdateLog "Editor: $EditorExecutable"
    Write-UpdateLog "Ready handshake: $ReadyFile"
    Write-BootstrapLog 'Startup parameters validated successfully.'
}
catch {
    $fatalMessage = "FATAL startup initialization error: $($_.Exception.Message)"
    Write-BootstrapLog $fatalMessage
    Write-UpdateLog $fatalMessage
    exit 9
}

function Quote-NativeArgument {
    param([string]$Value)

    if ($null -eq $Value) {
        return '""'
    }

    return '"' + $Value.Replace('"', '\"') + '"'
}

function Write-ProcessOutputToLog {
    param(
        [string]$StdOut,
        [string]$StdErr
    )

    foreach ($line in ($StdOut -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            Write-UpdateLog $line
        }
    }

    foreach ($line in ($StdErr -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            Write-UpdateLog $line
        }
    }
}

function Start-ManagedProcess {
    param(
        [string]$FileName,
        [string]$Arguments,
        [string]$NextState,
        [string]$Status,
        [string]$Detail
    )

    Set-Status -Status $Status -Detail $Detail
    Write-UpdateLog "Starting: $FileName $Arguments"

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FileName
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.WorkingDirectory = Split-Path -Parent $ProjectFile
    # Environment expansion in cmd avoids interpolating project paths into shell
    # syntax. These values remain quoted and are not recursively expanded.
    $startInfo.EnvironmentVariables['PD3_BUILD_BATCH'] = Join-Path $EngineDirectory 'Build\BatchFiles\Build.bat'
    $startInfo.EnvironmentVariables['PD3_BUILD_PROJECT'] = $ProjectFile

    $script:ActiveProcess = New-Object System.Diagnostics.Process
    $script:ActiveProcess.StartInfo = $startInfo

    if (-not $script:ActiveProcess.Start()) {
        throw "Could not start $FileName."
    }

    $script:ActiveStdOutTask = $script:ActiveProcess.StandardOutput.ReadToEndAsync()
    $script:ActiveStdErrTask = $script:ActiveProcess.StandardError.ReadToEndAsync()
    $script:State = $NextState
}

function Complete-ActiveProcess {
    $stdOut = $script:ActiveStdOutTask.Result
    $stdErr = $script:ActiveStdErrTask.Result
    $exitCode = $script:ActiveProcess.ExitCode

    Write-ProcessOutputToLog -StdOut $stdOut -StdErr $stdErr
    Write-UpdateLog "Process exited with code $exitCode."

    $result = [PSCustomObject]@{
        ExitCode = $exitCode
        StdOut = $stdOut
        StdErr = $stdErr
    }

    $script:ActiveProcess.Dispose()
    $script:ActiveProcess = $null
    $script:ActiveStdOutTask = $null
    $script:ActiveStdErrTask = $null
    return $result
}


# Run all backup and Git work outside the WPF dispatcher.
function Invoke-UpdateGit {
    param([string[]]$GitArguments)
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = 'git.exe'
    $info.Arguments = '-C ' + (Quote-NativeArgument $RepositoryRoot) + ' ' + (($GitArguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    Write-UpdateLog "git $($GitArguments -join ' ')"
    $proc = [Diagnostics.Process]::Start($info)
    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $stderr = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    $output = $stdout.Result
    $errors = $stderr.Result
    $code = $proc.ExitCode
    $proc.Dispose()
    Write-UpdateLog "Git exit code: $code. $errors"
    if ($code -ne 0) { throw "Git failed ($code): $($GitArguments -join ' '). $errors" }
    return $output
}
function Get-GitPaths {
    param([string[]]$GitArguments)
    return @((Invoke-UpdateGit $GitArguments).Split([char]0) | Where-Object { $_.Length -gt 0 })
}
function Save-Checkpoint {
    param($Checkpoint)
    $temp = $script:CheckpointPath + '.tmp'
    $Checkpoint | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temp -Encoding UTF8
    if (Test-Path -LiteralPath $script:CheckpointPath) { [IO.File]::Replace($temp, $script:CheckpointPath, ($script:CheckpointPath + '.previous')) }
    else { [IO.File]::Move($temp, $script:CheckpointPath) }
    Write-UpdateLog "Stage: $($Checkpoint.Stage)"
}
function Assert-RepositoryReady {
    if ((Invoke-UpdateGit @('ls-files','-u')).Length -gt 0) { throw 'Existing Git conflicts: resolve them and stage the resolutions first. No new update attempted.' }
    foreach ($operation in @('MERGE_HEAD','CHERRY_PICK_HEAD','REVERT_HEAD','rebase-merge','rebase-apply','sequencer','index.lock')) {
        $path = (Invoke-UpdateGit @('rev-parse','--git-path',$operation)).Trim()
        if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $RepositoryRoot $path }
        if (Test-Path -LiteralPath $path) { throw "Incomplete Git operation: $operation. Complete or abort it manually first." }
    }
}
function Test-GeneratedPath {
    param([string]$Path)
    # Preserve plugin binaries and ThirdParty dependencies, custom source and assets.
    return $Path -match '(^|/)(Intermediate|DerivedDataCache)/' -or $Path -match '^Binaries/'
}
function Invoke-RepositoryUpdate {
    Assert-EditorClosed
    $builders = @(Get-CimInstance Win32_Process -Filter "Name = 'dotnet.exe' OR Name = 'UnrealBuildTool.exe' OR Name = 'cmd.exe'" -ErrorAction Stop)
    foreach ($builder in $builders) {
        if ($builder.CommandLine -and $builder.CommandLine.IndexOf($ProjectFile, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and $builder.CommandLine -match 'UnrealBuildTool|Build\.bat') {
            throw 'A build for this project is still running. Wait for it to exit before continuing.'
        }
    }
    Assert-RepositoryReady
    $branch = (Invoke-UpdateGit @('branch','--show-current')).Trim()
    if ($branch -ne $BranchName) { throw "Expected branch $BranchName; found $branch." }
    $head = (Invoke-UpdateGit @('rev-parse','HEAD')).Trim()
    $remoteUrl = (Invoke-UpdateGit @('remote','get-url',$RemoteName)).Trim()
    if (Test-Path -LiteralPath $script:CheckpointPath) {
        $cp = Get-Content -LiteralPath $script:CheckpointPath -Raw | ConvertFrom-Json
        if ($cp.Stage -ne 'EditorReady') {
            if ($cp.Project -ne $ProjectFile -or $cp.Engine -ne $EngineDirectory -or $cp.Repository -ne $RepositoryRoot -or $cp.Branch -ne $branch -or $cp.Remote -ne $RemoteName -or $cp.RemoteUrl -ne $remoteUrl) { throw 'Pending update belongs to a different repository/branch/remote. Review checkpoint manually.' }
            if ($cp.Stage -eq 'SnapshotReady' -and $head -eq $cp.OriginalRevision) {
                # Fetch/preflight failed before cleanup began. Retain this recovery
                # record and snapshot, then safely retry with a fresh backup.
                Move-Item -LiteralPath $script:CheckpointPath -Destination (Join-Path $cp.Backup 'PreparationCheckpoint.json')
                Write-UpdateLog 'Retrying preparation; previous backup and snapshot retained.'
                Invoke-RepositoryUpdate
                return
            }
            if ($cp.Stage -eq 'UpdatingRepository' -and $head -eq $cp.TargetRevision -and $head -ne $cp.OriginalRevision) {
                $cp.Revision = $head
                $cp.Stage = 'RepositoryUpdated'
                Save-Checkpoint $cp
            }
            if ($cp.Stage -in @('SnapshotReady','UpdatingRepository')) { throw "Interrupted repository preparation. Recover manually before starting another update. Backup: $($cp.Backup); snapshot: $($cp.Snapshot); checkpoint: $script:CheckpointPath" }
            if ($cp.Revision -ne $head) { throw 'HEAD changed since the pending update; inspect the checkpoint before resuming.' }
            if ($cp.Stage -in @('RepositoryUpdated','Restoring','NeedsReview')) {
                $ack = Join-Path $cp.Backup 'Reviewed.txt'
                if (-not (Test-Path -LiteralPath $ack)) { throw "Repository updated; restoration needs review. Read $($cp.Backup)\Recovery.txt, reconcile files and stage resolutions, then create $ack and click Continue / retry rebuild. Do not apply the complete snapshot." }
                $cp.Stage = 'LocalChangesRestored'
                Save-Checkpoint $cp
            }
            Write-UpdateLog "Resuming $($cp.Stage); no repeated download or restoration."
            return
        }
    }
    $id = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
    $backup = Join-Path $backupBase $id
    New-Item -ItemType Directory -Path $backup | Out-Null
    $cp = [pscustomobject]@{ Stage='Preparing'; Repository=$RepositoryRoot; Branch=$branch; Remote=$RemoteName; RemoteUrl=$remoteUrl; Project=$ProjectFile; Engine=$EngineDirectory; OriginalRevision=$head; Revision=$head; TargetRevision=''; Snapshot=''; Backup=$backup; Review=@() }
    Invoke-UpdateGit @('status','--porcelain=v1','-z','--ignored','--untracked-files=all') | Set-Content -LiteralPath (Join-Path $backup 'Status.txt') -Encoding UTF8
    Invoke-UpdateGit @('remote','-v') | Set-Content -LiteralPath (Join-Path $backup 'Remotes.txt')
    $index = (Invoke-UpdateGit @('rev-parse','--git-path','index')).Trim()
    if (-not [IO.Path]::IsPathRooted($index)) { $index = Join-Path $RepositoryRoot $index }
    Copy-Item -LiteralPath $index -Destination (Join-Path $backup 'Original.index')
    $repoPrefix = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if ([IO.Path]::GetFullPath($backup).StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must be outside the repository.' }
    $files = Join-Path $backup 'Files'
    # Retain Content backup plus ignored dependencies, mods, configuration and
    # binaries. Never mirror/delete backup files or follow junctions.
    & robocopy.exe $RepositoryRoot $files /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /XJ /XD .git /XF .git /NP /NFL /NDL | ForEach-Object { Write-UpdateLog $_ }
    if ($LASTEXITCODE -ge 8) { throw 'Safety backup failed; no working tree changes made.' }
    $changed = @(@(Get-GitPaths @('diff','--name-only','--no-renames','-z','HEAD')) + @(Get-GitPaths @('diff','--cached','--name-only','--no-renames','-z','HEAD')) | Select-Object -Unique)
    $untracked = @(Get-GitPaths @('ls-files','--others','--exclude-standard','-z'))
    $ignored = @(Get-GitPaths @('ls-files','--others','--ignored','--exclude-standard','-z'))
    foreach ($path in @($changed + $untracked + $ignored | Select-Object -Unique)) {
        $local = Join-Path $RepositoryRoot $path
        if (Test-Path -LiteralPath $local -PathType Leaf) {
            $copy = Join-Path $files $path
            if (-not (Test-Path -LiteralPath $copy -PathType Leaf) -or (Get-FileHash -LiteralPath $local).Hash -ne (Get-FileHash -LiteralPath $copy).Hash) { throw "Backup verification failed: $path. No update attempted." }
        }
    }
    $snapshot = (Invoke-UpdateGit @('stash','create',"CrimeFactory update $id")).Trim()
    if ($changed.Count -gt 0 -and -not $snapshot) { throw 'Tracked snapshot failed.' }
    if ($snapshot) {
        $cp.Snapshot = "refs/crimefactory-updates/$id"
        Invoke-UpdateGit @('update-ref',$cp.Snapshot,$snapshot) | Out-Null
        if ((Invoke-UpdateGit @('rev-parse',$cp.Snapshot)).Trim() -ne $snapshot) { throw 'Snapshot verification failed.' }
    }
    @"
Original revision: $head
Snapshot: $($cp.Snapshot) (retained; unrelated stashes untouched).
Original.index retains staging. Do NOT copy it over the updated index.
Files\ contains original Content, mods, source, configuration and ignored dependencies.
Inspect git diff "$($cp.Snapshot)^1" "$($cp.Snapshot)" and staged tree "$($cp.Snapshot)^2".
Do not apply the entire snapshot over migrated plugins.
If interrupted during cleanup before Git updated: compare changed paths with Files\ and the snapshot;
restore/reconcile original work manually (do not blindly apply a partially restored snapshot).
Once the original work is recovered, move the Git-directory crimefactory-update.json into this
backup directory and retry the updater. Do not delete the recovery ref or backup.
A failed download before cleanup retries automatically with another verified backup.
For restoration review: reconcile the listed paths against Files\, resolve/stage conflicts,
then create Reviewed.txt here and click Continue / retry rebuild to explicitly accept your resolutions.
Staging is restored only for paths unchanged upstream; other staging remains in the snapshot/index.
"@ | Set-Content -LiteralPath (Join-Path $backup 'Recovery.txt')
    $protectedIndexHash = (Get-FileHash -LiteralPath $index).Hash
    $cp.Stage = 'SnapshotReady'
    Save-Checkpoint $cp
    Invoke-UpdateGit @('fetch',$RemoteName,$BranchName) | Out-Null
    $target = (Invoke-UpdateGit @('rev-parse','FETCH_HEAD')).Trim()
    Invoke-UpdateGit @('merge-base','--is-ancestor',$head,$target) | Out-Null
    Write-UpdateLog "Update download completed: $target"
    $incoming = @(Get-GitPaths @('ls-tree','-r','--name-only','-z',$target))
    $incomingSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $incomingDirectories = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $incoming) {
        [void]$incomingSet.Add($entry)
        $parent = $entry
        while ($parent.Contains('/')) {
            $parent = $parent.Substring(0, $parent.LastIndexOf('/'))
            [void]$incomingDirectories.Add($parent)
        }
    }
    @('Tracked paths to reconcile if interrupted:') + $changed + @('Untracked/ignored paths (see Files and Status.txt):') + $untracked + $ignored | Add-Content -LiteralPath (Join-Path $backup 'Recovery.txt')
    $cp.TargetRevision = $target
    $cp.Stage = 'UpdatingRepository'
    Save-Checkpoint $cp
    if ((Get-FileHash -LiteralPath $index).Hash -ne $protectedIndexHash) { throw 'The Git index changed during the update download. Stopped before cleanup.' }
    foreach ($path in $changed) {
        $local = Join-Path $RepositoryRoot $path
        $copy = Join-Path $files $path
        if ((Test-Path -LiteralPath $local -PathType Leaf) -ne (Test-Path -LiteralPath $copy -PathType Leaf)) { throw "File changed since backup: $path" }
        if ((Test-Path -LiteralPath $local -PathType Leaf) -and (Get-FileHash -LiteralPath $local).Hash -ne (Get-FileHash -LiteralPath $copy).Hash) { throw "File changed since backup: $path" }
    }
    foreach ($path in $changed) {
        Invoke-UpdateGit @('--literal-pathspecs','restore','--source=HEAD','--staged','--worktree','--',$path) | Out-Null
    }
    $review = New-Object 'System.Collections.Generic.List[string]'
    # Retire plugin roots whose upstream descriptors disappeared. User-authored
    # extras in those roots are quarantined, not carried into obsolete modules.
    $oldPlugins = @(Get-GitPaths @('ls-tree','-r','--name-only','-z',$head) | Where-Object { $_ -like 'Plugins/*.uplugin' })
    $newPluginRoots = @($incoming | Where-Object { $_ -like 'Plugins/*.uplugin' } | ForEach-Object { $_.Substring(0, $_.LastIndexOf('/') + 1) })
    $retiredRoots = @($oldPlugins | ForEach-Object { $_.Substring(0, $_.LastIndexOf('/') + 1) } | Where-Object { $newPluginRoots -notcontains $_ } | Select-Object -Unique)
    foreach ($path in @($untracked + $ignored | Select-Object -Unique)) {
        $collision = $incomingSet.Contains($path) -or $incomingDirectories.Contains($path)
        $parent = $path
        while (-not $collision -and $parent.Contains('/')) {
            $parent = $parent.Substring(0, $parent.LastIndexOf('/'))
            $collision = $incomingSet.Contains($parent)
        }
        $retired = @($retiredRoots | Where-Object { $path.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
        if ($collision -or $retired) {
            $local = Join-Path $RepositoryRoot $path
            if (Test-Path -LiteralPath $local -PathType Leaf) {
                $identical = $false
                if ($incomingSet.Contains($path)) {
                    $identical = (Invoke-UpdateGit @('hash-object','--path',$path,'--',$local)).Trim() -eq (Invoke-UpdateGit @('rev-parse',"$($target):$path")).Trim()
                }
                if (-not $identical -and -not (Test-GeneratedPath $path)) { $review.Add($path) }
                if ((Get-FileHash -LiteralPath $local).Hash -ne (Get-FileHash -LiteralPath (Join-Path $files $path)).Hash) { throw "File changed since backup: $path. Stopped without removing it." }
                Remove-Item -LiteralPath $local
            }
        }
    }
    Invoke-UpdateGit @('merge','--ff-only',$target) | Out-Null
    $cp.Revision = (Invoke-UpdateGit @('rev-parse','HEAD')).Trim()
    $cp.Stage = 'RepositoryUpdated'
    Save-Checkpoint $cp
    $cp.Stage = 'Restoring'
    Save-Checkpoint $cp
    foreach ($path in $changed) {
        if (Test-GeneratedPath $path) { continue }
        $old = Invoke-UpdateGit @('--literal-pathspecs','ls-tree',$head,'--',$path)
        $new = Invoke-UpdateGit @('--literal-pathspecs','ls-tree',$target,'--',$path)
        $localTree = Invoke-UpdateGit @('--literal-pathspecs','ls-tree',$snapshot,'--',$path)
        if ($old -eq $new) {
            Invoke-UpdateGit @('--literal-pathspecs','restore',"--source=$snapshot",'--worktree','--',$path) | Out-Null
            Invoke-UpdateGit @('--literal-pathspecs','restore',"--source=$($snapshot)^2",'--staged','--',$path) | Out-Null
        } elseif ($localTree -eq $new -or (-not $localTree -and -not $new)) {
            # Never transfer deletion of an old path onto an upstream rename.
            Write-UpdateLog "Local change already represented upstream: $path"
        } else { $review.Add($path) }
    }
    $cp.Review = @($review | Select-Object -Unique)
    if ($cp.Review.Count -gt 0) {
        $cp.Stage = 'NeedsReview'
        Save-Checkpoint $cp
        $cp.Review | Add-Content -LiteralPath (Join-Path $backup 'Recovery.txt')
        throw "Repository updated, but local changes need review: $($cp.Review -join ', '). Originals: $backup. Read Recovery.txt. No snapshot deleted."
    }
    $cp.Stage = 'LocalChangesRestored'
    Save-Checkpoint $cp
}
function Assert-EditorClosed {
    if ($null -ne (Get-Process -Id $EditorPid -ErrorAction SilentlyContinue)) {
        throw 'The original Unreal Editor is still running. Close it before retrying the rebuild.'
    }
    # Detect another editor opened on this project during the update. Fail closed
    # if process inspection is unavailable rather than cleaning locked binaries.
    $projectName = [IO.Path]::GetFileName($ProjectFile)
    $editors = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%.exe'" -ErrorAction Stop)
    foreach ($editor in $editors) {
        if ([string]::IsNullOrWhiteSpace($editor.CommandLine) -or
            $editor.CommandLine.IndexOf($projectName, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw 'An Unreal Editor may have this project open. Close it before retrying the rebuild.'
        }
    }
}


if ($RepositoryWorker) {
    try {
        $gitDirectory = (Invoke-UpdateGit @('rev-parse','--absolute-git-dir')).Trim()
        $script:CheckpointPath = Join-Path $gitDirectory 'crimefactory-update.json'
        $workerLock = [IO.File]::Open((Join-Path $gitDirectory 'crimefactory-update-worker.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try { Invoke-RepositoryUpdate } finally { $workerLock.Dispose() }
        exit 0
    } catch {
        Write-UpdateLog "Repository/recovery failed: $($_.Exception.Message)"
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
}

try {
    Add-Type -AssemblyName PresentationFramework
    Add-Type -AssemblyName PresentationCore
    Add-Type -AssemblyName WindowsBase
}
catch {
    $fatalMessage = "FATAL: Could not load WPF assemblies: $($_.Exception.Message)"
    Write-BootstrapLog $fatalMessage
    Write-UpdateLog $fatalMessage
    exit 10
}

$windowXaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="CrimeFactory - PD3 Modkit Updater" Width="820" Height="660" MinWidth="680" MinHeight="480"
        ResizeMode="CanResize" WindowStartupLocation="CenterScreen" ShowInTaskbar="True" Topmost="True"
        Background="#242424" Foreground="#DDDDDD" FontFamily="Segoe UI" FontSize="13">
    <Window.Resources>
        <Style TargetType="TextBlock"><Setter Property="Foreground" Value="#DDDDDD"/></Style>
        <Style TargetType="Button">
            <Setter Property="Background" Value="#383838"/><Setter Property="Foreground" Value="#EEEEEE"/>
            <Setter Property="BorderBrush" Value="#111111"/><Setter Property="BorderThickness" Value="1"/>
            <Setter Property="Padding" Value="12,6"/><Setter Property="Margin" Value="0,0,8,8"/>
            <Setter Property="Template"><Setter.Value>
                <ControlTemplate TargetType="Button">
                    <Border x:Name="Chrome" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                            BorderThickness="{TemplateBinding BorderThickness}" CornerRadius="2" Padding="{TemplateBinding Padding}">
                        <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                    </Border>
                    <ControlTemplate.Triggers>
                        <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Chrome" Property="Background" Value="#4B4B4B"/></Trigger>
                        <Trigger Property="IsPressed" Value="True"><Setter TargetName="Chrome" Property="Background" Value="#0069CB"/></Trigger>
                        <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger>
                    </ControlTemplate.Triggers>
                </ControlTemplate>
            </Setter.Value></Setter>
        </Style>
    </Window.Resources>
    <Grid Margin="24">
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/><RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>
        <StackPanel Grid.Row="0" Margin="0,0,0,18">
            <TextBlock Text="CRIMEFACTORY  /  MODKIT UPDATE" FontSize="11" Foreground="#AAAAAA"/>
            <TextBlock Text="PD3 Modkit" FontSize="32" FontWeight="Bold" Margin="0,8,0,8"/>
            <TextBlock Name="StatusText" Text="Waiting for Unreal Editor to close..." FontWeight="SemiBold" Foreground="#FFBC00" TextWrapping="Wrap"/>
        </StackPanel>
        <UniformGrid Grid.Row="1" Columns="3" Margin="0,0,0,18">
            <Border Background="#1B1B1B" Padding="16,12" Margin="0,0,6,0"><StackPanel>
                <TextBlock Name="BranchText" Text="Loading" FontSize="20" FontWeight="SemiBold" TextWrapping="Wrap"/>
                <TextBlock Text="Branch" Margin="0,5,0,0" Foreground="#AAAAAA"/>
            </StackPanel></Border>
            <Border Background="#1B1B1B" Padding="16,12" Margin="0,0,6,0"><StackPanel>
                <TextBlock Name="BackupStatusText" Text="Pending" FontSize="20" FontWeight="SemiBold" TextWrapping="Wrap"/>
                <TextBlock Text="Content backup" Margin="0,5,0,0" Foreground="#AAAAAA"/>
            </StackPanel></Border>
            <Border Background="#1B1B1B" Padding="16,12"><StackPanel>
                <TextBlock Text="After update" FontSize="20" FontWeight="SemiBold" TextWrapping="Wrap"/>
                <TextBlock Text="Automatic project reopening" Margin="0,5,0,0" Foreground="#AAAAAA" TextWrapping="Wrap"/>
            </StackPanel></Border>
        </UniformGrid>
        <StackPanel Grid.Row="2" Margin="0,0,0,14">
            <TextBlock Name="WarningText" Text="Please do not reopen the project while the update is in progress." TextWrapping="Wrap" Margin="0,0,0,12"/>
            <ProgressBar Name="ProgressBar" Height="8" IsIndeterminate="True" Foreground="#0070E0" Background="#1B1B1B" BorderThickness="0"/>
        </StackPanel>
        <Grid Grid.Row="3">
            <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/></Grid.RowDefinitions>
            <TextBlock Text="Current activity" FontWeight="SemiBold" Margin="0,0,0,8"/>
            <TextBox Grid.Row="1" Name="DetailText" IsReadOnly="True" TextWrapping="Wrap" VerticalScrollBarVisibility="Auto"
                     Text="The update will begin automatically as soon as Unreal Editor has fully exited."
                     Background="#1B1B1B" Foreground="#DDDDDD" BorderThickness="0" Padding="12"/>
        </Grid>
        <WrapPanel Grid.Row="4" Name="ButtonPanel" HorizontalAlignment="Right" Visibility="Collapsed" Margin="0,18,0,0">
            <Button Name="CopyReportButton" Content="Copy report"/>
            <Button Name="OpenBackupButton" Content="Open recovery backup" Visibility="Collapsed"/>
            <Button Name="ViewLogButton" Content="View update log"/>
            <Button Name="ReopenButton" Content="Reopen project" Background="#0070E0" Visibility="Collapsed"/>
            <Button Name="RetryRebuildButton" Content="Continue / retry rebuild" Visibility="Collapsed"/>
            <Button Name="CloseButton" Content="Close"/>
        </WrapPanel>
    </Grid>
</Window>
"@

try {
    $xmlReader = New-Object System.Xml.XmlNodeReader ([xml]$windowXaml)
    $window = [System.Windows.Markup.XamlReader]::Load($xmlReader)
}
catch {
    $fatalMessage = "FATAL: Could not create updater window: $($_.Exception.Message)"
    Write-BootstrapLog $fatalMessage
    Write-UpdateLog $fatalMessage
    exit 11
}

$window.FindName('BranchText').Text = $BranchName
$backupStatusText = $window.FindName('BackupStatusText')
$progressBar = $window.FindName('ProgressBar')
$statusText = $window.FindName('StatusText')
$detailText = $window.FindName('DetailText')
$warningText = $window.FindName('WarningText')
$buttonPanel = $window.FindName('ButtonPanel')
$openBackupButton = $window.FindName('OpenBackupButton')
$viewLogButton = $window.FindName('ViewLogButton')
$reopenButton = $window.FindName('ReopenButton')
$retryRebuildButton = $window.FindName('RetryRebuildButton')
$closeButton = $window.FindName('CloseButton')

function Set-Status {
    param(
        [string]$Status,
        [string]$Detail
    )

    $statusText.Text = $Status
    $detailText.Text = $Detail
    if ($script:BackupCreated) { $backupStatusText.Text = 'Ready'; $backupStatusText.Foreground = '#20DB5D' }
    elseif (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $ProjectFile) 'Content'))) { $backupStatusText.Text = 'No Content folder' }
    else { $backupStatusText.Text = 'Pending' }
}

function Show-Failure {
    param(
        [string]$Status,
        [string]$Detail,
        [bool]$CanRetryReopen
    )

    Write-UpdateLog "$Status $Detail"
    $script:State = 'Failed'
    $statusText.Foreground = '#FF4040'
    $script:AllowClose = $true
    $progressBar.IsIndeterminate = $false
    $progressBar.Value = 0
    $warningText.Text = 'The updater has stopped. Review the message below before reopening the project.'
    Set-Status -Status $Status -Detail $Detail
    $buttonPanel.Visibility = 'Visible'
    $retryRebuildButton.Visibility = if ($script:RebuildPending) { 'Visible' } else { 'Collapsed' }

    if ($script:BackupCreated -and (Test-Path -LiteralPath $script:BackupRoot)) {
        $openBackupButton.Visibility = 'Visible'
    }
    else {
        $openBackupButton.Visibility = 'Collapsed'
    }

    if ($CanRetryReopen) {
        $reopenButton.Visibility = 'Visible'
    }
    else {
        $reopenButton.Visibility = 'Collapsed'
    }
}

function Start-Editor {
    if ($script:PullSucceeded -and -not $script:UpdateCompleted) {
        Write-UpdateLog 'Editor launch blocked: the updated project has not completed its rebuild.'
        return $false
    }
    try {
        Write-UpdateLog "Reopening project: $ProjectFile"

        $launchInfo = New-Object System.Diagnostics.ProcessStartInfo
        $launchInfo.FileName = $EditorExecutable
        $launchInfo.Arguments = '"' + $ProjectFile.Replace('"', '\"') + '"'
        $launchInfo.WorkingDirectory = Split-Path -Parent $ProjectFile
        $launchInfo.UseShellExecute = $true

        $launched = [System.Diagnostics.Process]::Start($launchInfo)
        if ($null -eq $launched) {
            throw 'The Unreal Editor process did not start.'
        }

        return $true
    }
    catch {
        Write-UpdateLog "REOPEN FAILED: $($_.Exception.Message)"
        return $false
    }
}

function Save-LastBackupPointer {
    try {
        if (-not $script:BackupCreated) {
            return
        }

        New-Item -ItemType Directory -Path $backupBase -Force | Out-Null
        $lastBackupFile = Join-Path $backupBase 'LastContentBackup.txt'
        $script:BackupRoot | Set-Content -LiteralPath $lastBackupFile -Encoding UTF8
    }
    catch {
        Write-UpdateLog "Could not write LastContentBackup.txt: $($_.Exception.Message)"
    }
}


function Start-ContentBackup {
    $script:AllowClose = $false
    $script:RebuildPending = $false
    $buttonPanel.Visibility = 'Collapsed'
    $progressBar.IsIndeterminate = $true
    $statusText.Foreground = '#FFBC00'
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File ' + (Quote-NativeArgument $PSCommandPath) + ' -ConfigPath ' + (Quote-NativeArgument $ConfigPath) + ' -RepositoryWorker'
    Start-ManagedProcess -FileName "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments $arguments -NextState 'UpdatingRepository' -Status 'Backing up and updating repository...' -Detail "Preserving local data and staging. Progress and recovery details: $LogPath"
}
function Finish-RepositoryWorker {
    $result = Complete-ActiveProcess
    $script:RebuildPending = $true
    if (Test-Path -LiteralPath $script:CheckpointPath) {
        $cp = Get-Content -LiteralPath $script:CheckpointPath -Raw | ConvertFrom-Json
        $script:BackupRoot = $cp.Backup
        $script:BackupCreated = Test-Path -LiteralPath $cp.Backup
    }
    if ($result.ExitCode -ne 0) {
        Show-Failure -Status 'Repository / local-change recovery needs attention.' -Detail ($result.StdErr + [Environment]::NewLine + "See $LogPath. After recovery, click Continue / retry rebuild.") -CanRetryReopen $false
        return
    }
    $cp = Get-Content -LiteralPath $script:CheckpointPath -Raw | ConvertFrom-Json
    $script:BackupRoot = $cp.Backup
    $script:BackupContentPath = Join-Path $cp.Backup 'Files\Content'
    $script:BackupCreated = $true
    Save-LastBackupPointer
    $script:PullSucceeded = $true
    Start-ProjectRebuild
}
function Show-RebuildFailure {
    param([string]$Message)
    Show-Failure -Status 'Update applied, but the C++ rebuild failed.' `
        -Detail "$Message`n`nFull build output: $LogPath`nFix the error and use Retry rebuild; the update will not be downloaded again." `
        -CanRetryReopen $false
}

function Start-UnrealStage {
    param([string]$Stage)

    Assert-EditorClosed
    Assert-RepositoryReady
    $cp = Get-Content -LiteralPath $script:CheckpointPath -Raw | ConvertFrom-Json
    if ($cp.Revision -ne (Invoke-UpdateGit @('rev-parse','HEAD')).Trim()) { throw 'HEAD changed during rebuild.' }
    $cp.Stage = $Stage
    Save-Checkpoint $cp
    $buildBatch = Join-Path $EngineDirectory 'Build\BatchFiles\Build.bat'
    if (-not (Test-Path -LiteralPath $buildBatch -PathType Leaf)) {
        throw "Unreal Build.bat was not found: $buildBatch"
    }
    if (-not (Test-Path -LiteralPath $ProjectFile -PathType Leaf)) {
        throw "Project file was not found: $ProjectFile"
    }

    # UBT treats the engine rules as read-only with -UsePrecompiled, including on
    # source builds. Clean then visits project/plugin outputs, not engine outputs.
    # UBT resolves all enabled project plugin dependencies from the editor target.
    $arguments = 'PAYDAY3Editor Win64 Development -Project="%PD3_BUILD_PROJECT%" -architecture=x64 -WaitMutex -UsePrecompiled -NoHotReloadFromIDE -NoEngineChanges'
    switch ($Stage) {
        'GeneratingProjectFiles' {
            $arguments = '-ProjectFiles -Project="%PD3_BUILD_PROJECT%" -Game -UsePrecompiled -WaitMutex'
            $status = 'Generating project files...'
        }
        'CleaningProject' {
            $arguments += ' -Clean -SkipPreBuildTargets'
            $status = 'Cleaning project binaries...'
        }
        'BuildingProject' { $status = 'Rebuilding C++ modules...' }
        default { throw "Unknown build stage: $Stage" }
    }
    # cmd requires the extra pair of outer quotes for a quoted batch path.
    Start-ManagedProcess -FileName "$env:SystemRoot\System32\cmd.exe" `
        -Arguments ('/D /V:OFF /S /C ""%PD3_BUILD_BATCH%" ' + $arguments + '"') `
        -NextState $Stage -Status $status `
        -Detail "Engine: $EngineDirectory`nTarget: PAYDAY3Editor / Win64 / Development Editor`nProject: $ProjectFile`n`nThis can take several minutes. Compiler output is captured in the update log when each stage exits.`n$LogPath"
}

function Start-ProjectRebuild {
    $script:RebuildPending = $true
    $script:UpdateCompleted = $false
    $script:AllowClose = $false
    $buttonPanel.Visibility = 'Collapsed'
    $reopenButton.Visibility = 'Collapsed'
    $statusText.Foreground = '#FFBC00'
    $progressBar.IsIndeterminate = $true
    $warningText.Text = 'Please keep Unreal Editor closed until the C++ rebuild completes.'
    try {
        Assert-EditorClosed
        Assert-RepositoryReady
        $cp = Get-Content -LiteralPath $script:CheckpointPath -Raw | ConvertFrom-Json
        if ($cp.Stage -notin @('LocalChangesRestored','ProjectValidated','GeneratingProjectFiles','CleaningProject','BuildingProject','EditorReady') -or $cp.Revision -ne (Invoke-UpdateGit @('rev-parse','HEAD')).Trim()) { throw 'Repository recovery incomplete or HEAD changed; review checkpoint before rebuilding.' }
        Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json | Out-Null
        $cp.Stage = 'ProjectValidated'
        Save-Checkpoint $cp
        Write-UpdateLog "Starting full project rebuild with engine: $EngineDirectory"
        Start-UnrealStage 'GeneratingProjectFiles'
    }
    catch { Show-RebuildFailure $_.Exception.Message }
}

function Finish-UnrealStage {
    $stage = $script:State
    $result = Complete-ActiveProcess
    if ($result.ExitCode -ne 0) {
        $output = $result.StdOut + "`n" + $result.StdErr
        $errors = @($output -split "`r?`n" | Where-Object { $_ -match '(?i)error|exception|failed|fatal' })
        $diagnostic = if ($errors.Count -gt 0) { ($errors | Select-Object -Last 12) -join "`n" } else { $output.Trim() }
        if ($diagnostic.Length -gt 5000) { $diagnostic = $diagnostic.Substring($diagnostic.Length - 5000) }
        Show-RebuildFailure "$stage exited with code $($result.ExitCode).`n$diagnostic"
        return
    }
    Write-UpdateLog "Stage completed: $stage"
    switch ($stage) {
        'GeneratingProjectFiles' { Start-UnrealStage 'CleaningProject' }
        'CleaningProject' { Start-UnrealStage 'BuildingProject' }
        'BuildingProject' {
            $cp = Get-Content -LiteralPath $script:CheckpointPath -Raw | ConvertFrom-Json
            $cp.Stage = 'EditorReady'
            Save-Checkpoint $cp
            $script:RebuildPending = $false
            $script:UpdateCompleted = $true
            Complete-UpdateAndScheduleReopen
        }
    }
}

function Complete-UpdateAndScheduleReopen {
    Save-LastBackupPointer
    Write-UpdateLog 'PD3 Modkit update, local-change restoration and full C++ rebuild completed successfully.'
    if ($script:BackupCreated) {
        Write-UpdateLog "Content safety backup: $($script:BackupRoot)"
    }

    $statusText.Foreground = '#20DB5D'
    Set-Status -Status 'Rebuild completed.' -Detail 'The update and full C++ rebuild succeeded. Your local changes have been restored. Reopening PD3 Modkit...'
    $warningText.Text = 'The update was installed successfully. The project will reopen automatically.'
    $script:ReopenAt = [DateTime]::UtcNow.AddMilliseconds(900)
    $script:State = 'ReopenPending'
}


try {
    $gitDirectory = (Invoke-UpdateGit @('rev-parse','--absolute-git-dir')).Trim()
    $script:CheckpointPath = Join-Path $gitDirectory 'crimefactory-update.json'
    $workerProbe = [IO.File]::Open((Join-Path $gitDirectory 'crimefactory-update-worker.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $workerProbe.Dispose()
    $script:UpdateLock = [IO.File]::Open((Join-Path $gitDirectory 'crimefactory-update.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
} catch {
    Write-UpdateLog "Cannot acquire updater lock: $($_.Exception.Message)"
    exit 12
}

Write-UpdateLog "Updater UI initialized. Waiting for Unreal Editor process $EditorPid to close."
Write-UpdateLog "Content backup base: $backupBase"

$window.Add_ContentRendered({
    if (-not $script:ReadySignaled) {
        try {
            $readyDirectory = Split-Path -Parent $ReadyFile
            if (-not [string]::IsNullOrWhiteSpace($readyDirectory)) {
                New-Item -ItemType Directory -Path $readyDirectory -Force | Out-Null
            }

            "ready" | Set-Content -LiteralPath $ReadyFile -Encoding ASCII
            $script:ReadySignaled = $true
            Write-UpdateLog 'Updater window rendered and readiness handshake written.'
        }
        catch {
            $fatalMessage = "FATAL: Could not write readiness handshake: $($_.Exception.Message)"
            Write-BootstrapLog $fatalMessage
            Write-UpdateLog $fatalMessage
            $script:AllowClose = $true
            $window.Close()
        }
    }
})

$window.FindName('CopyReportButton').Add_Click({
    try {
        [System.Windows.Clipboard]::SetText(($statusText.Text + "`r`n`r`n" + $detailText.Text + "`r`n`r`nUpdate log: " + $LogPath + "`r`nBackup: " + $script:BackupRoot))
    } catch { Write-UpdateLog "Could not copy the report: $($_.Exception.Message)" }
})

$openBackupButton.Add_Click({
    try {
        if ($script:BackupCreated -and (Test-Path -LiteralPath $script:BackupRoot)) {
            Start-Process -FilePath 'explorer.exe' -ArgumentList ('"' + $script:BackupRoot + '"')
        }
    }
    catch {
    }
})

$viewLogButton.Add_Click({
    try {
        Start-Process -FilePath 'notepad.exe' -ArgumentList ('"' + $LogPath + '"')
    }
    catch {
    }
})

$reopenButton.Add_Click({
    if (Start-Editor) {
        $script:AllowClose = $true
        $window.Close()
    }
    else {
        Set-Status -Status 'The project could not be reopened.' -Detail 'You can close this window and reopen the .uproject manually.'
    }
})

$retryRebuildButton.Add_Click({ Start-ContentBackup })

$closeButton.Add_Click({
    $script:AllowClose = $true
    $window.Close()
})

$window.Add_Closing({
    param($sender, $eventArgs)

    if (-not $script:AllowClose) {
        $eventArgs.Cancel = $true
    }
})

$timer = New-Object System.Windows.Threading.DispatcherTimer
$timer.Interval = [TimeSpan]::FromMilliseconds(250)
$timer.Add_Tick({
    try {
        switch ($script:State) {
            'WaitingForEditor' {
                $editorStillRunning = $null -ne (Get-Process -Id $EditorPid -ErrorAction SilentlyContinue)

                if (-not $editorStillRunning) {
                    Write-UpdateLog 'Unreal Editor has exited.'
                    Set-Status -Status 'Preparing update...' -Detail 'Unreal Editor has closed. The project Content folder will be backed up before Git changes anything.'
                    $script:EditorClosedAt = [DateTime]::UtcNow
                    $script:State = 'EditorClosedDelay'
                }
            }

            'EditorClosedDelay' {
                if (([DateTime]::UtcNow - $script:EditorClosedAt).TotalMilliseconds -ge 750) {
                    Start-ContentBackup
                }
            }


            'UpdatingRepository' {
                if ($null -ne $script:ActiveProcess -and $script:ActiveProcess.HasExited -and $script:ActiveStdOutTask.IsCompleted -and $script:ActiveStdErrTask.IsCompleted) { Finish-RepositoryWorker }
            }

            { $_ -in @('GeneratingProjectFiles', 'CleaningProject', 'BuildingProject') } {
                if ($null -ne $script:ActiveProcess -and $script:ActiveProcess.HasExited -and
                    $script:ActiveStdOutTask.IsCompleted -and $script:ActiveStdErrTask.IsCompleted) {
                    Finish-UnrealStage
                }
            }

            'ReopenPending' {
                if ([DateTime]::UtcNow -ge $script:ReopenAt) {
                    if (Start-Editor) {
                        Write-UpdateLog 'Editor relaunch requested successfully.'
                        $script:AllowClose = $true
                        $timer.Stop()
                        $window.Close()
                    }
                    else {
                        Show-Failure -Status 'Update installed, but the project could not be reopened.' -Detail 'The update completed successfully. Use Reopen Project to retry, or close this window and launch the .uproject manually.' -CanRetryReopen $true
                    }
                }
            }
        }
    }
    catch {
        if ($script:RebuildPending) { Show-RebuildFailure $_.Exception.Message }
        else { Show-Failure -Status 'Updater error.' -Detail "$($_.Exception.Message)" -CanRetryReopen $script:UpdateCompleted }
    }
})

$timer.Start()
try { [void]$window.ShowDialog() } finally { $script:UpdateLock.Dispose() }
$timer.Stop()
Write-BootstrapLog 'Updater script exited.'
