#
# Wrapper script Symbol Server
#
param(
	[string]$DriverName = "xenbus",
	[string]$SymbolServer = "c:\symbols",
	[switch]$Free,
	[switch]$Checked
)

Function Add-Symbols {
	param(
		[string]$DriverName,
		[string]$DriverPath,
		[string]$SymbolServer,
		[string]$Arch
	)

	$cwd = Get-Location
	Set-Location $DriverPath

	$symstore = [string]::Format("{0}Debuggers\{1}\symstore.exe", $env:WDKContentRoot, $Arch)

	$inffile=[string]::Format("{0}.inf", $DriverName)
	$Version=(Get-Content -Path $inffile | Select-String "DriverVer").Line.Split(',')[1]

	Write-Host $symstore "add" "/s" $SymbolServer "/r" "/f" "*.pdb" "/t" $DriverName "/v" $Version
	Get-ChildItem -Path "." -Include "*.pdb" -Name | Write-Host
	& $symstore "add" "/s" $SymbolServer "/r" "/f" "*.pdb" "/t" $DriverName "/v" $Version

	Set-Location $cwd
}

if ($Free -or -not $Checked) {
	$driverpath = [string]::Format("{0}\x64", $DriverName)
	Add-Symbols $DriverName $driverpath $SymbolServer "x64"
	$driverpath = [string]::Format("{0}\x86", $DriverName)
	Add-Symbols $DriverName $driverpath $SymbolServer "x86"
}
if ($Checked) {
	$driverpath = [string]::Format("{0}-checked\x64", $DriverName)
	Add-Symbols $DriverName $driverpath $SymbolServer "x64"
	$driverpath = [string]::Format("{0}-checked\x86", $DriverName)
	Add-Symbols $DriverName $driverpath $SymbolServer "x86"
}
