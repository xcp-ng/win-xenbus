#
# Main build script
#

param(
	[Parameter(Mandatory = $true)]
	[string]$Type,
	[string]$Arch,
	[switch]$CodeQL,
	[switch]$Sdv
)

#
# Script Body
#

Function Build {
	param(
		[string]$Arch,
		[string]$Type
	)

	$visualstudioversion = $Env:VisualStudioVersion
	$solutiondir = @{ "14.0" = "vs2015"; "15.0" = "vs2017"; "16.0" = "vs2019"; }
	$configurationbase = @{ "14.0" = "Windows 8"; "15.0" = "Windows 8"; "16.0" = "Windows 8"; }

	$params = @{
		SolutionDir = $solutiondir[$visualstudioversion];
		ConfigurationBase = $configurationbase[$visualstudioversion];
		Arch = $Arch;
		Type = $Type
		}
	& ".\msbuild.ps1" @params
	if ($LASTEXITCODE -ne 0) {
		Write-Host -ForegroundColor Red "ERROR: Build failed, code:" $LASTEXITCODE
		Exit $LASTEXITCODE
	}
}

Function SdvBuild {
	$visualstudioversion = $Env:VisualStudioVersion
	$solutiondir = @{ "14.0" = "vs2015"; "15.0" = "vs2017"; "16.0" = "vs2019"; }
	$configurationbase = @{ "14.0" = "Windows 10"; "15.0" = "Windows 10"; "16.0" = "Windows 10"; }
	$arch = "x64"

	$params = @{
		SolutionDir = $solutiondir[$visualstudioversion];
		ConfigurationBase = $configurationbase[$visualstudioversion];
		Arch = $arch;
		Type = "sdv"
		}
	& ".\msbuild.ps1" @params
}

function CodeQLBuild {
	$visualstudioversion = $Env:VisualStudioVersion
	$solutiondir = @{ "14.0" = "vs2015"; "15.0" = "vs2017"; "16.0" = "vs2019"; }
	$configurationbase = @{ "14.0" = "Windows 10"; "15.0" = "Windows 10"; "16.0" = "Windows 10"; }
	$arch = "x64"

	$params = @{
		SolutionDir = $solutiondir[$visualstudioversion];
		ConfigurationBase = $configurationbase[$visualstudioversion];
		Arch = $arch;
		Type = "codeql"
		}
	& ".\msbuild.ps1" @params
}

if ($Type -ne "free" -and $Type -ne "checked") {
	Write-Host "Invalid Type"
	Exit -1
}

if ([string]::IsNullOrEmpty($Env:VENDOR_NAME)) {
	Set-Item -Path Env:VENDOR_NAME -Value 'Xen Project'
}

if ([string]::IsNullOrEmpty($Env:VENDOR_PREFIX)) {
	Set-Item -Path Env:VENDOR_PREFIX -Value 'XP'
}

if ([string]::IsNullOrEmpty($Env:PRODUCT_NAME)) {
	Set-Item -Path Env:PRODUCT_NAME -Value 'Xen'
}

if ([string]::IsNullOrEmpty($Env:BUILD_NUMBER)) {
	if (Test-Path ".build_number") {
		$BuildNum = Get-Content -Path ".build_number"
		Set-Content -Path ".build_number" -Value ([int]$BuildNum + 1)
	} else {
		$BuildNum = '0'
		Set-Content -Path ".build_number" -Value '1'
	}
	Set-Item -Path Env:BUILD_NUMBER -Value $BuildNum
}

if ([string]::IsNullOrEmpty($Env:MAJOR_VERSION)) {
	Set-Item -Path Env:MAJOR_VERSION -Value '9'
}

if ([string]::IsNullOrEmpty($Env:MINOR_VERSION)) {
	Set-Item -Path Env:MINOR_VERSION -Value '1'
}

if ([string]::IsNullOrEmpty($Env:MICRO_VERSION)) {
	Set-Item -Path Env:MICRO_VERSION -Value '0'
}

if ([string]::IsNullOrEmpty($Arch) -or $Arch -eq "x86" -or $Arch -eq "Win32") {
	Build "x86" $Type
}

if ([string]::IsNullOrEmpty($Arch) -or $Arch -eq "x64") {
	Build "x64" $Type
}

if ($CodeQL) {
	CodeQLBuild
}

if ($Sdv) {
	SdvBuild
}
