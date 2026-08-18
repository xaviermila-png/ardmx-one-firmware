$ports = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'COM\d+' }

foreach ($p in $ports) {
    Write-Output ("LIST:" + $p.Name)
}

$candidate = $ports | Where-Object {
    $_.Name -match 'CH340|CH9102|CP210|FTDI|Silicon Labs|USB-SERIAL|USB UART'
} | Select-Object -First 1

if ($candidate -and $candidate.Name -match '\((COM\d+)\)') {
    Write-Output ("RECOMMENDED:" + $matches[1])
}
