Function Parse-Tags {
    param(
        [string]$drivername
    )

    Get-ChildItem ("./src/" + $drivername) | Foreach-Object {
        $file = $_.Name
        Get-Content $_.FullName | ForEach {
            if ($_.Contains("TAG") -And $_.Contains("#define")) {
                $vals = $_.Split(' ', 3)
                $name = $vals[1].Trim()
                $tags = $vals[2].Trim().Trim("'").PadRight(4)
                Write-Host "TAG:" $name "=" $tags
                $driver = ($drivername + ".sys").PadRight(16)
                ($tags + " - " + $driver + " - XEN " + $drivername + "\" + $file + " " + $name) | Add-Content "./pooltag.txt"
            }
        }
    }
}

if (Test-Path "./pooltag.txt") {
    Remove-Item "./pooltag.txt"
}
Get-ChildItem "./src" | ?{$_.PSIsContainer}  | ForEach-Object {
    Parse-Tags $_.Name
}
