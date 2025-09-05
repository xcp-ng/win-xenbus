#
# Main build script
#

param(
	[Parameter(Mandatory = $true)]
	[string]$Type,
	[string]$Arch,
	[string]$SignMode = "TestSign",
	[switch]$CodeQL,
	[switch]$Sdv,
	[switch]$CodeAnalysis
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
	$solutiondir = @{ "16.0" = "vs2019"; "17.0" = "vs2022"; }
	$configurationbase = @{ "16.0" = "Windows 10"; "17.0" = "Windows 10"; }

	$params = @{
		SolutionDir = $solutiondir[$visualstudioversion];
		ConfigurationBase = $configurationbase[$visualstudioversion];
		Arch = $Arch;
		Type = $Type;
		SignMode = $SignMode;
		CodeAnalysis = $CodeAnalysis
		}
	& ".\msbuild.ps1" @params
	if ($LASTEXITCODE -ne 0) {
		Write-Host -ForegroundColor Red "ERROR: Build failed, code:" $LASTEXITCODE
		Exit $LASTEXITCODE
	}
}

Function SdvBuild {
	$visualstudioversion = $Env:VisualStudioVersion
	$solutiondir = @{ "16.0" = "vs2019"; "17.0" = "vs2022"; }
	$configurationbase = @{ "16.0" = "Windows 10"; "17.0" = "Windows 10"; }
	$arch = "x64"

	$params = @{
		SolutionDir = $solutiondir[$visualstudioversion];
		ConfigurationBase = $configurationbase[$visualstudioversion];
		Arch = $arch;
		Type = "sdv";
		SignMode = $SignMode
		}
	& ".\msbuild.ps1" @params
}

function CodeQLBuild {
	$visualstudioversion = $Env:VisualStudioVersion
	$solutiondir = @{ "16.0" = "vs2019"; "17.0" = "vs2022"; }
	$configurationbase = @{ "16.0" = "Windows 10"; "17.0" = "Windows 10"; }
	$arch = "x64"

	$params = @{
		SolutionDir = $solutiondir[$visualstudioversion];
		ConfigurationBase = $configurationbase[$visualstudioversion];
		Arch = $arch;
		Type = "codeql";
		SignMode = $SignMode
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

if ([string]::IsNullOrEmpty($Env:COPYRIGHT)) {
	Set-Item -Path Env:COPYRIGHT -Value 'Copyright (c) Xen Project.'
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
	if ($Env:VisualStudioVersion -ne "17.0") {
		Build "x86" $Type
	}
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
