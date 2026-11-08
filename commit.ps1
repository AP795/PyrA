# Commit all Pyra changes, then push the current branch to its configured upstream.
# Usage: .\commit.ps1 "Commit message" (unquoted message words also work).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$message = ($args -join ' ').Trim()
if (-not $message) {
	throw 'Usage: .\commit.ps1 "Commit message"'
}

$root = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $root '.git'))) {
	throw "No Git repository found at $root"
}

function Invoke-Git
{
	& git -C $root @args
	if ($LASTEXITCODE -ne 0) {
		throw "git ($($args -join ' ')) failed with exit code $LASTEXITCODE"
	}
}

# Inherited Git variables must not redirect commands to another repository.
$savedGitEnvironment = @{}
try {
	foreach ($name in 'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_PREFIX', 'GIT_COMMON_DIR') {
		$savedGitEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
		Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
	}

# Check the branch and upstream before staging or committing anything.
	$branch = Invoke-Git symbolic-ref --quiet --short HEAD
	$upstream = Invoke-Git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}'

	Invoke-Git add --all
	& git -C $root diff --cached --quiet
	if ($LASTEXITCODE -eq 1) {
		Invoke-Git commit --message $message
	} elseif ($LASTEXITCODE -eq 0) {
		Write-Host 'Nothing to commit.'
	} else {
		throw "git diff --cached failed with exit code $LASTEXITCODE"
	}

# Always try the push: a previous attempt may have left local commits unpushed.
	Write-Host "Pushing $branch to $upstream..."
	Invoke-Git push
} finally {
	foreach ($name in $savedGitEnvironment.Keys) {
		if ([string]::IsNullOrEmpty($savedGitEnvironment[$name])) {
			Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
		} else {
			[Environment]::SetEnvironmentVariable($name, $savedGitEnvironment[$name], 'Process')
		}
	}
}
