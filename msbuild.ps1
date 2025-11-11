#
# Wrapper script for MSBuild
#
param(
	[string]$SolutionDir = "vs2019",
	[string]$ConfigurationBase = "Windows 10",
	[Parameter(Mandatory = $true)]
	[string]$Arch,
	[Parameter(Mandatory = $true)]
	[string]$Type,
	[string]$SignMode = "TestSign",
	[switch]$CodeAnalysis
)

#
# Globals
#
$SolutionName = "xenbus.sln"
$ArchivePath = "xenbus"
$ProjectList = @( "xen", "xenfilt", "xenbus" )

#
# Functions
#
Function Run-MSBuild {
	param(
		[string]$SolutionPath,
		[string]$Name,
		[string]$Configuration,
		[string]$Platform,
		[string]$Target = "Build",
		[string]$Inputs = "",
		[switch]$CodeAnalysis
	)

	$c = "msbuild.exe"
	$c += " /m:4"
	$c += [string]::Format(" /p:Configuration=""{0}""", $Configuration)
	$c += [string]::Format(" /p:Platform=""{0}""", $Platform)
	$c += [string]::Format(" /p:SignMode=""{0}""", $SignMode)
	$c += [string]::Format(" /t:""{0}"" ", $Target)
	if ($Inputs) {
		$c += [string]::Format(" /p:Inputs=""{0}"" ", $Inputs)
	}
	if ($CodeAnalysis) {
		$c += "/p:RunCodeAnalysis=true "
		$c += "/p:EnablePREFast=true "
	}

	$c += Join-Path -Path $SolutionPath -ChildPath $Name

	Invoke-Expression $c
	if ($LASTEXITCODE -ne 0) {
		throw "ERROR: MSBuild failed, code: $LASTEXITCODE"
	}
}

Function Run-MSBuildSDV {
	param(
		[string]$SolutionPath,
		[string]$Name,
		[string]$Configuration,
		[string]$Platform
	)

	$basepath = Get-Location
	$versionpath = Join-Path -Path $SolutionPath -ChildPath "version"
	$projpath = Join-Path -Path $SolutionPath -ChildPath $Name
	Set-Location $projpath

	$project = [string]::Format("{0}.vcxproj", $Name)
	Run-MSBuild $versionpath "version.vcxproj" $Configuration $Platform "Build"
	Run-MSBuild $projpath $project $Configuration $Platform "Build"
	Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/clean"
	Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/check:default.sdv /debug"

	Set-Location $basepath
}

Function Run-MSBuildDVL {
	param(
		[string]$SolutionPath,
		[string]$Name,
		[string]$Configuration,
		[string]$Platform
	)

	$basepath = Get-Location
	$projpath = Join-Path -Path $SolutionPath -ChildPath $Name
	Set-Location $projpath

	$project = [string]::Format("{0}.vcxproj", $Name)

	Run-MSBuild $projpath $project $Configuration $Platform "Build" -CodeAnalysis
	Run-MSBuild $projpath $project $Configuration $Platform "dvl"

	$refine = Join-Path -Path $projpath -ChildPath "refine.sdv"
	if (Test-Path -Path $refine -PathType Leaf) {
		Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/refine"
	}

	Copy-Item "*DVL*" -Destination $SolutionPath

	Set-Location $basepath
}

Function Run-CodeQL {
	param(
		[string]$SolutionPath,
		[string]$Name,
		[string]$Configuration,
		[string]$Platform,
		[string]$SearchPath,
		[string]$QueryFile
	)

	$projpath = Resolve-Path (Join-Path $SolutionPath $Name)
	$project = [string]::Format("{0}.vcxproj", $Name)
	$output = [string]::Format("{0}.sarif", $Name)
	$database = Join-Path "database" $Name
	$sarif = Join-Path $projpath $output

	# write a bat file to wrap msbuild parameters
	$bat = [string]::Format("{0}.bat", $Name)
	if (Test-Path $bat) {
		Remove-Item $bat
	}
	$a = "msbuild.exe"
	$a += " /m:4"
	$a += " /t:Rebuild"
	$a += [string]::Format(" /p:Configuration=""{0}""", $Configuration)
	$a += [string]::Format(" /p:Platform=""{0}""", $Platform)
	$a += [string]::Format(" /p:SignMode=""{0}""", $SignMode)
	$a += " "
	$a += Join-Path $projpath $project
	$a | Set-Content $bat

	# generate the database
	$b = "codeql"
	$b += " database"
	$b += " create"
	$b += " -l=cpp"
	$b += " -s=src"
	$b += " -c"
	$b += ' "' + (Resolve-Path $bat) + '" '
	$b += $database
	Invoke-Expression $b
	if ($LASTEXITCODE -ne 0) {
		throw "ERROR: CodeQL failed, code: $LASTEXITCODE"
	}
	Remove-Item $bat

	# perform the analysis on the database
	$c = "codeql"
	$c += " database"
	$c += " analyze "
	$c += $database
	$c += " "
	$c += $QueryFile
	$c += " --format=sarifv2.1.0"
	$c += " --threads=0"
	$c += " --output="
	$c += $sarif
	$c += " --search-path="
	$c += $SearchPath

	Invoke-Expression $c
	if ($LASTEXITCODE -ne 0) {
		throw "ERROR: CodeQL failed, code: $LASTEXITCODE"
	}

	Copy-Item $sarif -Destination $SolutionPath
}

#
# Script Body
#

$configuration = @{ "free" = "$ConfigurationBase Release"; "checked" = "$ConfigurationBase Debug"; "sdv" = "$ConfigurationBase Release"; "codeql" = "$ConfigurationBase Release"; }
$platform = @{ "x86" = "Win32"; "x64" = "x64" }
$solutionpath = Resolve-Path $SolutionDir

Set-ExecutionPolicy -Scope CurrentUser -Force Bypass

if (-Not (Test-Path -Path $archivepath)) {
	New-Item -Name $archivepath -ItemType Directory | Out-Null
}

if (($Type -eq "free") -or ($Type -eq "checked")) {
	Run-MSBuild $solutionpath $SolutionName $configuration[$Type] $platform[$Arch] -CodeAnalysis:$CodeAnalysis
}

if ($Type -eq "codeql") {
	if ([string]::IsNullOrEmpty($Env:CODEQL_QUERY_SUITE)) {
		$searchpath = Resolve-Path ".."
	} else {
		$searchpath = $Env:CODEQL_QUERY_SUITE
	}

	if (Test-Path "database") {
		Remove-Item -Recurse -Force "database"
	}
	New-Item -ItemType Directory "database"

	$queryfile = "windows_driver_recommended.qls"
	Try {
		$ver = New-Object System.Version((& "codeql" "--version")[0].Split(" ")[-1] + "0")
		Write-Host -ForegroundColor Cyan "INFO: CodeQL version " $ver
		$minver = New-Object System.Version("2.20.1.0")
		if ($ver -ge $minver) {
			$queryfile = "mustfix.qls"
		}
	} Catch {
	}
	if (-not [string]::IsNullOrEmpty($Env:CODEQL_QUERY_FILE)) {
		$queryfile = $Env:CODEQL_QUERY_FILE
		Write-Host -ForegroundColor Cyan "INFO: Overwriting codeql query file to " $queryfile
	}
	ForEach ($project in $ProjectList) {
		Run-CodeQL $solutionpath $project $configuration["codeql"] $platform[$Arch] $searchpath $queryfile
	}
	Copy-Item -Path (Join-Path -Path $SolutionPath -ChildPath "*.sarif") -Destination $archivepath
}

if ($Type -eq "sdv") {
	ForEach ($project in $ProjectList) {
		Run-MSBuildSDV $solutionpath $project $configuration["sdv"] $platform[$Arch]
	}
}

if (($Type -eq "codeql") -or ($Type -eq "sdv")) {
	ForEach ($project in $ProjectList) {
		Run-MSBuildDVL $solutionpath $project $configuration["sdv"] $platform[$Arch]
	}
	Copy-Item -Path (Join-Path -Path $SolutionPath -ChildPath "*DVL*") -Destination $archivepath
}
