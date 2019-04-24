#
# Generate version.h and xenbus.inf
#
param(
	[string]$Platform = "Win32",
	[string]$SolutionDir = "vs2017",
	[string]$IncludeDir = "include",
	[string]$SourceDir = "src"
)

# Copy $InFileName -> $OutFileName replacing $Token$_.Key$Token with $_.Value from
# $Replacements
Function Copy-FileWithReplacements {
	param(
		[Parameter(Mandatory = $true)]
		[string]$InFileName,
		[Parameter(Mandatory = $true)]
		[string]$OutFileName,
		[hashtable]$Replacements,
		[string]$Token = "@"
	)

	Write-Host "Copy-FileWithReplacements"
	Write-Host $InFileName" -> "$OutFileName

	(Get-Content $InFileName) |
	ForEach-Object {
		$line = $_
		$Replacements.GetEnumerator() | ForEach-Object {
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
$TheYear = Get-Date -UFormat "%Y"
$TheMonth = Get-Date -UFormat "%m"
$TheDay = Get-Date -UFormat "%d"
$InfArch = @{ "Win32" = "x86"; "x64" = "amd64" }
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
	# values determined from the build environment
	'VENDOR_NAME' = $Env:VENDOR_NAME;
	'PRODUCT_NAME' = $Env:PRODUCT_NAME;
	'VENDOR_DEVICE_ID' = $Env:VENDOR_DEVICE_ID;
	'VENDOR_PREFIX' = $Env:VENDOR_PREFIX;

	'MAJOR_VERSION' = $Env:MAJOR_VERSION;
	'MINOR_VERSION' = $Env:MINOR_VERSION;
	'MICRO_VERSION' = $Env:MICRO_VERSION;

	# generated values
	'BUILD_NUMBER' = $TheBuildNum;
	'GIT_REVISION' = $GitRevision;

	'INF_DATE' = $InfDate;
	'INF_ARCH' = $InfArch[$Platform];
	'YEAR' = $TheYear;
	'MONTH' = $TheMonth;
	'DAY' = $TheDay
}

$Replacements | Out-String | Write-Host

$includepath = Resolve-Path $IncludeDir
$src = Join-Path -Path $includepath -ChildPath "version.tmpl"
$dst = Join-Path -Path $includepath -ChildPath "version.h"
Copy-FileWithReplacements $src $dst -Replacements $Replacements

$sourcepath = Resolve-Path $SourceDir
$solutionpath = Resolve-Path $SolutionDir
$src = Join-Path -Path $sourcepath -ChildPath "xenbus.inf"
$dst = Join-Path -Path $solutionpath -ChildPath "xenbus.inf"
Copy-FileWithReplacements $src $dst -Replacements $Replacements
