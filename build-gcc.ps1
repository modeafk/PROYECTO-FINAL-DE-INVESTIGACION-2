$ErrorActionPreference = 'Stop'

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
    -o cheetahtraj-spatiotemporal.exe

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

Write-Host 'Compilacion y pruebas completadas.'
