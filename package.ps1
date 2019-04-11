#
# Package - create the output package
#
param(
	[string]$SolutionDir = "vs2017",
	[string]$DriverName = "xenbus",
	[string]$ConfigurationBase = "Windows10",
	[switch]$Free,
	[switch]$Checked
)

Function Build-Package {
	param(
		[string]$SolutionDir,
		[string]$BinPath,
		[string]$Package
	)

	$zipfile = [string]::Format("{0}.zip", $Package)
	$hashfile = [string]::Format("{0}.sha256", $Package)
	if (Test-Path -Path $zipfile) {
		Remove-Item -Path $zipfile -Recurse -Force
	}
	if (Test-Path -Path $hashfile) {
		Remove-Item -Path $hashfile -Recurse -Force
	}
	if (Test-Path -Path $Package) {
		Remove-Item -Path $Package -Recurse -Force
	}
	New-Item -Name $Package -ItemType Directory | Out-Null

	$src = Join-Path -Path $BinPath -ChildPath "x64\package\*"
	$dst = Join-Path -Path $Package -ChildPath "x64"
	New-Item -Path $dst -ItemType Directory | Out-Null
	Copy-Item -Path $src -Destination $dst -Recurse -Force

	$src = Join-Path -Path $BinPath -ChildPath "Win32\package\*"
	$dst = Join-Path -Path $Package -ChildPath "x86"
	New-Item -Path $dst -ItemType Directory | Out-Null
	Copy-Item -Path $src -Destination $dst -Recurse -Force

	Copy-Item ".build_number" $Package
	Copy-Item ".revision" $Package

	Get-ChildItem -Path $SolutionDir -Include "*.DVL.XML" -File -Recurse | Copy-Item -Destination $Package

	Compress-Archive -Path $Package -DestinationPath $zipfile

	$hash = Get-FileHash -Path $zipfile -Algorithm SHA256
	$hash.Hash | Set-Content $hashfile

	Format-List -InputObject $hash
}

#
# Script Body
#

if ($Free -or -not $Checked) {
	$config=[string]::Format("{0}Release", $ConfigurationBase);
	$binpath = Join-Path -Path $SolutionDir -ChildPath $config
	Build-Package $SolutionDir $binpath $DriverName
}
if ($Checked) {
	$config=[string]::Format("{0}Debug", $ConfigurationBase);
	$binpath = Join-Path -Path $SolutionDir -ChildPath $config
	$package = [string]::Format("{0}-checked", $DriverName)
	Build-Package $SolutionDir $binpath $package
}
