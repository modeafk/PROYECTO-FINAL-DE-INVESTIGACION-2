param(
    [string]$Dataset = 'C:\Users\User\Documents\UCSP\2026-01\PFC1\Tema_investigacion\IMPLEMENTACION\implementacion1\archive\train.csv',
    [ValidateRange(1, 2147483647)]
    [int]$Limite,
    [switch]$SoloCompilar
)

$ErrorActionPreference = 'Stop'

if (-not $SoloCompilar) {
    $datasetPath = (Get-Item -LiteralPath $Dataset).FullName
}
$compileTimer = [System.Diagnostics.Stopwatch]::StartNew()

$compiler = 'C:\msys64\ucrt64\bin\g++.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    $compiler = (Get-Command g++ -ErrorAction Stop).Source
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/io.cpp src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp src/temporal.cpp src/main.cpp `
    -o cheetahtraj-cli.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-cli.exe'
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp tests/test_core.cpp `
    -o cheetahtraj-tests.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-tests.exe'
}

& .\cheetahtraj-tests.exe
if ($LASTEXITCODE -ne 0) {
    throw 'Las pruebas no pasaron'
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/io.cpp src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp `
    src/temporal.cpp src/temporal_invquad.cpp tests/test_temporal.cpp `
    -o cheetahtraj-temporal-tests.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-temporal-tests.exe'
}

& .\cheetahtraj-temporal-tests.exe
if ($LASTEXITCODE -ne 0) {
    throw 'Las pruebas temporales no pasaron'
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/io.cpp src/projection.cpp src/vqgs.cpp src/temporal.cpp src/temporal_main.cpp `
    -o cheetahtraj-temporal.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-temporal.exe'
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/io.cpp src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp `
    src/temporal.cpp src/temporal_invquad.cpp src/spatiotemporal_main.cpp `
    -lpsapi -o cheetahtraj-spatiotemporal.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-spatiotemporal.exe'
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/io.cpp src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp src/temporal.cpp `
    src/viewer_win32.cpp -lgdi32 -luser32 -o cheetahtraj-viewer.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-viewer.exe'
}

& $compiler -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -Iinclude `
    src/io.cpp src/projection.cpp src/vqgs.cpp src/quadtree.cpp src/invquad.cpp `
    src/temporal.cpp src/temporal_invquad.cpp src/temporal_visual_main.cpp `
    -lgdi32 -luser32 -o cheetahtraj-temporal-visual-color.exe

if ($LASTEXITCODE -ne 0) {
    throw 'No se pudo compilar cheetahtraj-temporal-visual-color.exe'
}

$compileTimer.Stop()
Write-Host ('Compilacion y pruebas completadas. Tiempo: {0:F2} s.' -f $compileTimer.Elapsed.TotalSeconds)
if ($SoloCompilar) { return }

$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$indexDir = Join-Path $PSScriptRoot "output\porto_st_index_$runId"
$limitArgs = @()
if ($PSBoundParameters.ContainsKey('Limite')) {
    $limitArgs = @('--limit', $Limite)
}

Write-Host "`nConstruyendo el indice temporal..."
& .\cheetahtraj-spatiotemporal.exe build --format porto --dataset $datasetPath `
    --index-dir $indexDir --window-size 3600 --window-step 3600 `
    --min-points 2 --height 2 --delta 4 --zoom 15 @limitArgs
if ($LASTEXITCODE -ne 0) { throw 'No se pudo construir el indice temporal' }

Write-Host "`nAbriendo el menu del visor con el indice generado..."
& .\cheetahtraj-temporal-visual-color.exe --index-dir $indexDir
if ($LASTEXITCODE -ne 0) { throw 'El visor temporal termino con un error' }
