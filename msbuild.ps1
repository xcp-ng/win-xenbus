#
# Wrapper script for MSBuild
#
param(
	[string]$SolutionDir = "vs2017",
	[string]$ConfigurationBase = "Windows 10",
	[Parameter(Mandatory = $true)]
	[string]$Arch,
	[Parameter(Mandatory = $true)]
	[string]$Type
)

Function Run-MSBuild {
	param(
		[string]$SolutionPath,
		[string]$Name,
		[string]$Configuration,
		[string]$Platform,
		[string]$Target = "Build",
		[string]$Inputs = ""
	)

	$c = [string]::Format("/p:Configuration=`"{0}`"", $Configuration)
	$p = [string]::Format("/p:Platform=`"{0}`"", $Platform)
	$t = [string]::Format("/t:`"{0}`"", $Target)
	$s = Join-Path -Path $SolutionPath -ChildPath $Name
	if ($Inputs) {
		$i = [string]::Format("/p:Inputs=`"{0}`"", $Inputs)
		Write-Host "msbuild.exe" "/m:1" $c $p $t $i $s
		& "msbuild.exe" "/m:1" $c $p $t $s $i
	} else {
		Write-Host "msbuild.exe" "/m:1" $c $p $t $s
		& "msbuild.exe" "/m:1" $c $p $t $s
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
	Run-MSBuild $projpath $project $Configuration $Platform "dvl"

	$refine = Join-Path -Path $projpath -ChildPath "refine.sdv"
	if (Test-Path -Path $refine -PathType Leaf) {
		Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/refine"
	}

	Copy-Item "*DVL*" -Destination $SolutionPath

	Set-Location $basepath
}

#
# Script Body
#

$configuration = @{ "free" = "$ConfigurationBase Release"; "checked" = "$ConfigurationBase Debug"; "sdv" = "$ConfigurationBase Release"; }
$platform = @{ "x86" = "Win32"; "x64" = "x64" }
$solutionpath = Resolve-Path $SolutionDir

if ($Type -eq "free") {
	Run-MSBuild $solutionpath "xenbus.sln" $configuration["free"] $platform[$Arch]
}
elseif ($Type -eq "checked") {
	Run-MSBuild $solutionpath "xenbus.sln" $configuration["checked"] $platform[$Arch]
}
elseif ($Type -eq "sdv") {
	$archivepath = "xenbus"

	if (-Not (Test-Path -Path $archivepath)) {
		New-Item -Name $archivepath -ItemType Directory | Out-Null
	}

	Run-MSBuildSDV $solutionpath "xen" $configuration["sdv"] $platform[$Arch]
	Run-MSBuildSDV $solutionpath "xenfilt" $configuration["sdv"] $platform[$Arch]
	Run-MSBuildSDV $solutionpath "xenbus" $configuration["sdv"] $platform[$Arch]

	Copy-Item -Path (Join-Path -Path $SolutionPath -ChildPath "*DVL*") -Destination $archivepath
}
