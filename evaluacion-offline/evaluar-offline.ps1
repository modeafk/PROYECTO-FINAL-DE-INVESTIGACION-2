param(
    [Parameter(Mandatory)]
    [string]$Dataset,
    [ValidateRange(1, 2147483647)]
    [int[]]$Limites = @(1000, 5000),
    [ValidateRange(1, 100)]
    [int]$Repeticiones = 5
)

$ErrorActionPreference = 'Stop'
$datasetPath = (Resolve-Path -LiteralPath $Dataset).Path
$compiler = 'C:\msys64\ucrt64\bin\g++.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    $compiler = (Get-Command g++ -ErrorAction Stop).Source
}
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$output = Join-Path $PSScriptRoot "resultados\$runId"
New-Item -ItemType Directory -Path $output | Out-Null
$exe = Join-Path $output 'cheetahtraj-offline-benchmark.exe'

function Get-Median([double[]]$Values) {
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if ($ordered.Count % 2 -eq 1) { return $ordered[$middle] }
    return ($ordered[$middle - 1] + $ordered[$middle]) / 2
}

Push-Location -LiteralPath $PSScriptRoot
try {
    & $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic -static-libgcc -static-libstdc++ -Iinclude `
        src/io.cpp src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp `
        src/temporal.cpp src/temporal_invquad.cpp src/offline_benchmark.cpp -lpsapi -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Fallo la compilacion del benchmark' }
    $compilerVersion = & $compiler --version | Select-Object -First 1
    $measurements = @(
        foreach ($limit in $Limites) {
            for ($repeat = 1; $repeat -le $Repeticiones; $repeat++) {
                $modes = if ($repeat % 2 -eq 1) { 'static', 'temporal' } else { 'temporal', 'static' }
                foreach ($mode in $modes) {
                    $destination = Join-Path $output "n$limit-$repeat-$mode"
                    $line = & $exe $mode $datasetPath $limit $destination
                    if ($LASTEXITCODE -ne 0) { throw "Fallo la medicion $destination" }
                    $sample = $line | ConvertFrom-Json
                    $sample | Add-Member -NotePropertyName repetition -NotePropertyValue $repeat
                    Write-Host ('n={0} corrida={1} {2,-8} construir={3,9:F2} ms guardar={4,7:F2} ms offline={5,9:F2} ms RAM pico={6,6:F2} MiB indice={7} bytes' -f `
                        $limit, $repeat, $mode, $sample.invquad_build_ms, $sample.invquad_save_ms,
                        $sample.offline_ms, ($sample.ram_peak_bytes / 1MB), $sample.index_bytes)
                    $sample
                }
            }
        }
    )
    $summary = @(
        foreach ($group in ($measurements | Group-Object trips, mode)) {
            $samples = $group.Group
            $first = $samples[0]
            [pscustomobject]@{
                Viajes = $first.trips
                Metodo = $first.mode
                Ventanas = $first.windows
                VentanasNoVacias = $first.nonempty_windows
                Segmentos = $first.segments
                Lectura_ms = [Math]::Round((Get-Median $samples.load_ms), 2)
                Construccion_ms = [Math]::Round((Get-Median $samples.invquad_build_ms), 2)
                GuardadoInvQuad_ms = [Math]::Round((Get-Median $samples.invquad_save_ms), 2)
                Offline_ms = [Math]::Round((Get-Median $samples.offline_ms), 2)
                RAM_pre_offline_MiB = [Math]::Round((Get-Median $samples.ram_before_offline_bytes) / 1MB, 2)
                RAM_pico_MiB = [Math]::Round((Get-Median $samples.ram_peak_bytes) / 1MB, 2)
                Indice_bytes = $first.index_bytes
                Salida_bytes = $first.output_bytes
            }
        }
    )
    @{
        dataset = $datasetPath
        compiler = $compilerVersion
        repetition_count = $Repeticiones
        settings = @{ zoom = 15; delta = 4; height = 2; window_seconds = 3600; step_seconds = 3600; minimum_points = 2 }
        measurements = $measurements
        summary = $summary
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'resultados.json') -Encoding utf8
    Write-Host "`nMedianas por metodo. RAM pico del proceso hasta completar offline. Tiempos en ms."
    $summary | Format-Table Viajes, Metodo, Construccion_ms, GuardadoInvQuad_ms, Offline_ms, RAM_pico_MiB, Indice_bytes, Salida_bytes -AutoSize
    Write-Host "Resultados: $output"
} finally {
    Pop-Location
}
