#
# Generate version.h and xenbus.inf
#
param(
	[string]$SolutionDir = "vs2017",
	[string]$ConfigFile = $null,
	[Parameter(Mandatory = $true)]
	[string]$Arch
)

# Copy $InFileName -> $OutFileName replacing $Token$_.Key$Token with $_.Value from
# either $ConfigFile or $Replacements
Function Copy-FileWithReplacements {
	param(
		[Parameter(Mandatory = $true)]
		[string]$InFileName,
		[Parameter(Mandatory = $true)]
		[string]$OutFileName,
		[string]$ConfigFile,
		[hashtable]$Replacements,
		[string]$Token = "@"
	)

	Write-Host "Copy-FileWithReplacements"
	Write-Host $InFileName" -> "$OutFileName

	if ($ConfigFile) {
		$List = Get-Content $ConfigFile | Out-String | iex
		$List | Out-String | Write-Host
	} elseif ($Replacements) {
		$List = $Replacements
	} else {
		Write-Host "Invalid Arguments, ConfigFile or Replacements must be set"
		Write-Host
		Exit -1
	}

	(Get-Content $InFileName) | 
	ForEach-Object {
		$line = $_
		$List.GetEnumerator() | ForEach-Object {
			$key = [string]::Format("{0}{1}{2}", $Token, $_.Name, $Token)
			if (([string]::IsNullOrEmpty($_.Value)) -and ($line.Contains($key))) {
				Write-Host "Skipping Line Containing " $_.Name
				$line = $null
			}
			$line = $line -replace $key, $_.Value
		}
		$line
	} |
	Set-Content $OutFileName
}

#
# Script Body
#
$cwd = Get-Location
Set-Location $PSScriptRoot

$TheYear = Get-Date -UFormat "%Y"
$TheMonth = Get-Date -UFormat "%m"
$TheDay = Get-Date -UFormat "%d"
$InfArch = @{ "x86" = "x86"; "x64" = "amd64" }
$InfDate = Get-Date -UFormat "%m/%d/%Y"

# if GitRevision is $null, GIT_REVISION will be excluded from the Copy-FileWithReplacements
$GitRevision = & "git.exe" "rev-list" "--max-count=1" "HEAD"
if ($GitRevision) {
	Set-Content -Path ".revision" -Value $GitRevision
}

# if ".build_number" doesnt exist, BUILD_NUMBER = 0
# since this can called by the vcxproj, do not autoincrement the build number
# as this will mean x64 and Win32 builds have different numbers!
if (Test-Path ".build_number") {
	$TheBuildNum = Get-Content -Path ".build_number"
} else {
	Set-Content -Path ".build_number" -Value "0"
}
if (-not $TheBuildNum) {
	$TheBuildNum = '0'
}

# [ordered] makes output easier to parse by humans
$Replacements = [ordered]@{
	# default parameters, may be overridden in config.ps1
	'VENDOR_NAME' = 'Xen Project';
	'PRODUCT_NAME' = 'Xen';
	'VENDOR_DEVICE_ID' = $null; # must define this replacement, or @VENDOR_DEVICE_ID@ will remain in OutFileName
	'VENDOR_PREFIX' = 'XP';

	'MAJOR_VERSION' = '9';
	'MINOR_VERSION' = '0';
	'MICRO_VERSION' = '0';

	# generated values (should not be in config.ps1)
	'BUILD_NUMBER' = $TheBuildNum;
	'GIT_REVISION' = $GitRevision;

	'INF_DATE' = $InfDate;
	'INF_ARCH' = $InfArch[$Arch];
	'YEAR' = $TheYear;
	'MONTH' = $TheMonth;
	'DAY' = $TheDay
}

if ($ConfigFile -and (Test-Path -Path $ConfigFile)) {
	$config = Resolve-Path $ConfigFile | Get-Content | Out-String | iex
	$config.GetEnumerator() | % { $Replacements[$_.Key] = $_.Value }
}

$Replacements | Out-String | Write-Host

$includepath = Resolve-Path "include"
$src = Join-Path -Path $includepath -ChildPath "version.tmpl"
$dst = Join-Path -Path $includepath -ChildPath "version.h"
Copy-FileWithReplacements $src $dst -Replacements $Replacements

$sourcepath = Resolve-Path "src"
$solutionpath = Resolve-Path $SolutionDir
$src = Join-Path -Path $sourcepath -ChildPath "xenbus.inf"
$dst = Join-Path -Path $solutionpath -ChildPath "xenbus.inf"
Copy-FileWithReplacements $src $dst -Replacements $Replacements

Set-Location $cwd
