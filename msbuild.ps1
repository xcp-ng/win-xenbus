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
	$projpath = Join-Path -Path $SolutionPath -ChildPath $Name
	Set-Location $projpath

	$project = [string]::Format("{0}.vcxproj", $Name)
	Run-MSBuild $projpath $project $Configuration $Platform "Build"
	Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/clean"
	Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/check:default.sdv /debug"
	Run-MSBuild $projpath $project $Configuration $Platform "dvl"

	$refine = Join-Path -Path $projpath -ChildPath "refine.sdv"
	if (Test-Path -Path $refine -PathType Leaf) {
		Run-MSBuild $projpath $project $Configuration $Platform "sdv" "/refine"
	}

	Set-Location $basepath
}

Function Copy-To-Archive {
	param(
		[string]$ArtifactPath,
		[string]$ArchivePath
	)

	if (-Not (Test-Path -Path $ArtifactPath)) {
                return
	}

	if (-Not (Test-Path -Path $ArchivePath)) {
		New-Item -Name $ArchivePath -ItemType Directory | Out-Null
	}

	$src = Join-Path -Path $ArtifactPath -ChildPath "package"
	Get-ChildItem -Path $src -File | Copy-Item -Destination $ArchivePath
}

#
# Script Body
#

$configuration = @{ "free" = "$ConfigurationBase Release"; "checked" = "$ConfigurationBase Debug"; "sdv" = "$ConfigurationBase Release"; }
$platform = @{ "x86" = "Win32"; "x64" = "x64" }
$solutionpath = Resolve-Path $SolutionDir
$artifactpath = Join-Path -Path $solutionpath -ChildPath (Join-Path -Path $configuration[$Type].Replace(' ', '') -Childpath $platform[$Arch])
$archivepath = Join-Path -Path (Resolve-Path "xenbus") -ChildPath $Arch

if ($Type -eq "free") {
	Run-MSBuild $solutionpath "xenbus.sln" $configuration["free"] $platform[$Arch]
	Copy-To-Archive $artifactpath $archivepath
}
elseif ($Type -eq "checked") {
	Run-MSBuild $solutionpath "xenbus.sln" $configuration["checked"] $platform[$Arch]
	Copy-To-Archive $artifactpath $archivepath
}
elseif ($Type -eq "sdv") {
	Run-MSBuildSDV $solutionpath "xen" $configuration["sdv"] $platform[$Arch]
	Run-MSBuildSDV $solutionpath "xenfilt" $configuration["sdv"] $platform[$Arch]
	Run-MSBuildSDV $solutionpath "xenbus" $configuration["sdv"] $platform[$Arch]

	Get-ChildItem -Path $artifactpath -Include "*.DVL.XML" -File -Recurse | Copy-Item -Destination $archivepath
}
