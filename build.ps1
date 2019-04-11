#
# Wrapper script for MSBuild
# Also creates the final package(s) (if specified) and bumps the build number
#
param(
	[string]$SolutionDir = "vs2017",
	[string]$DriverName = "xenbus",
	[string]$ConfigurationBase = "Windows 10",
	[switch]$Free,
	[switch]$Checked,
	[switch]$Sdv,
	[switch]$Package,
	[switch]$DontBumpBuild
)

Function Run-MSBuild {
	param(
		[string]$SolutionDir,
		[string]$SolutionName,
		[string]$Configuration,
		[string]$Platform,
		[string]$Target = "Build",
		[string]$Inputs = ""
	)

	$c=[string]::Format("/p:Configuration=`"{0}`"", $Configuration)
	$p=[string]::Format("/p:Platform=`"{0}`"", $Platform)
	$t=[string]::Format("/t:`"{0}`"", $Target)
	$s=[string]::Format("{0}\{1}", $SolutionDir, $SolutionName)
	if ($Inputs) {
		$i=[string]::Format("/p:Inputs=`"{0}`"", $Inputs)
		Write-Host "msbuild.exe" "/m:1" $c $p $t $i $s
		& "msbuild.exe" "/m:1" $c $p $t $s $i
	} else {
		Write-Host "msbuild.exe" "/m:1" $c $p $t $s
		& "msbuild.exe" "/m:1" $c $p $t $s
	}
}

Function Run-MSBuildSDV {
	param(
		[string]$SolutionDir,
		[string]$ProjectName
	)

	$basepath = Get-Location
	$projpath = Join-Path -Path $SolutionDir -ChildPath $ProjectName
	Set-Location $projpath

	$project = [string]::Format("{0}.vcxproj", $ProjectName)
	Run-MSBuild $projpath $project "Windows 10 Release" "x64" "Build"
	Run-MSBuild $projpath $project "Windows 10 Release" "x64" "sdv" "/clean"
	Run-MSBuild $projpath $project "Windows 10 Release" "x64" "sdv" "/check:default.sdv /debug"
	Run-MSBuild $projpath $project "Windows 10 Release" "x64" "dvl"

	$refine = Join-Path -Path $projpath -ChildPath "refine.sdv"
	if (Test-Path -Path $refine -PathType Leaf) {
		Run-MSBuild $projpath $project "Windows 10 Release" "x64" "sdv" "/refine"
	}

	Set-Location $basepath
}

#
# Script Body
#

$configuration = @{ "free"="$ConfigurationBase Release"; "checked"="$ConfigurationBase Debug" }
$solutionname = [string]::Format("{0}.sln", $DriverName)
$solutiondir = Resolve-Path $SolutionDir

if ($Free -or -not ($Sdv -or $Checked)) {
	Run-MSBuild $solutiondir $solutionname $configuration["free"] "x64"
	Run-MSBuild $solutiondir $solutionname $configuration["free"] "Win32"
}
if ($Checked) {
	Run-MSBuild $solutiondir $solutionname $configuration["checked"] "x64"
	Run-MSBuild $solutiondir $solutionname $configuration["checked"] "Win32"
}
if ($Sdv) {
	Run-MSBuildSDV $solutiondir "xen"
	Run-MSBuildSDV $solutiondir "xenfilt"
	Run-MSBuildSDV $solutiondir "xenbus"
}
if ($Package) {
	$config=$ConfigurationBase.Replace(' ', '')
	$params = @{
		SolutionDir=$SolutionDir;
		DriverName=$DriverName;
		ConfigurationBase=$config;
		Free=$Free;
		Checked=$Checked
	}
	& ".\package.ps1" @params
}
if (-not $DontBumpBuild) {
	if (Test-Path ".build_number") {
		$TheBuildNum = Get-Content -Path ".build_number"
		Set-Content -Path ".build_number" -Value ([int]$TheBuildNum + 1)
	} else {
		Set-Content -Path ".build_number" -Value "1"
	}
}
