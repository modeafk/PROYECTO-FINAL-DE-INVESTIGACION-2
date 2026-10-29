# Baseline temporal

Esta variante conserva VQGS+ sin memoria y lo ejecuta independientemente en
ventanas temporales. Su objetivo es producir el baseline necesario para medir
la inestabilidad entre ventanas antes de introducir el parámetro de memoria.

## Formato temporal

El cargador espera un CSV ordenable por timestamp:

```csv
trajectory_id,longitude,latitude,timestamp
1001,-8.6400,41.1400,1704096000
1001,-8.6395,41.1405,1704096900
```

`timestamp` se expresa en segundos Unix. Las filas se agrupan por
`trajectory_id` y los puntos de cada trayectoria se ordenan por tiempo.

## Semántica de ventanas

Las ventanas son semiabiertas `[start, end)`. Un punto ubicado exactamente en
`end` pertenece a la ventana siguiente, evitando duplicados en ventanas
consecutivas sin solapamiento.

Cada segmento recibe un `id` local consecutivo para VQGS+, pero conserva el
`source_id` del dataset. Este identificador estable permitirá calcular más
adelante la similitud de Jaccard entre selecciones consecutivas.

## Ejecución de ejemplo

```powershell
.\build-gcc.ps1
.\cheetahtraj-temporal.exe
```

Para otro dataset:

```powershell
.\cheetahtraj-temporal.exe `
  --dataset data\mi_dataset_temporal.csv `
  --window-size 3600 `
  --window-step 3600 `
  --count 25 `
  --delta 64
```

Resultados:

- `window_summary.csv`: cantidad de trayectorias, muestra, Q y tiempo por ventana.
- `window_selections.csv`: selección ordenada y `source_id` por ventana.

Esta etapa todavía no incluye memoria, lambda ni estabilidad. Es
intencionalmente el comportamiento independiente de VQGS+ que servirá como
comparación para la propuesta.

## Modos de selección

El modo de presupuesto fijo maximiza Q con una cantidad o tasa determinada:

```powershell
.\cheetahtraj-temporal.exe ... --count 10 --quality-target 0
.\cheetahtraj-temporal.exe ... --rate 0.1
```

El modo predeterminado utiliza `alpha = 0.005`. Al indicar `--rate`, la tasa
reemplaza cualquier cantidad fija y en cada ventana se calcula
`k_j = ceil(alpha * |T_j|)`. Por ejemplo, `--rate 0.1` selecciona
aproximadamente el 10 % de las trayectorias de cada ventana.

El modo de calidad objetivo selecciona una cantidad variable en cada ventana
hasta alcanzar el umbral solicitado:

```powershell
.\cheetahtraj-temporal.exe ... --quality-target 0.80
```

Cuando `quality-target` es mayor que cero, `count` y `rate` no determinan el
tamaño final. `window_summary.csv` registra la cantidad seleccionada, la tasa
resultante, la calidad, el objetivo y si se alcanzó.

## Dataset original de Porto

También se admite el `train.csv` original con columnas `TRIP_ID`, `TIMESTAMP`,
`MISSING_DATA` y `POLYLINE`. `TIMESTAMP` es el inicio del viaje y al punto de
índice `i` se le asigna `TIMESTAMP + 15*i`, porque el dataset registra una
posición cada 15 segundos.

El archivo completo ocupa aproximadamente 1.94 GB. Para evitar agotar la
memoria, el ejecutable exige `--limit` o un rango `--from/--to` cuando se usa
`--format porto`:

```powershell
.\cheetahtraj-temporal.exe `
  --format porto `
  --dataset "C:\ruta\train.csv" `
  --limit 5000 `
  --window-size 3600 `
  --window-step 3600 `
  --rate 0.10 `
  --delta 64 `
  --output output\porto_temporal_5k
```

Por defecto se omiten las filas con `MISSING_DATA=True` y las polilíneas con
menos de dos puntos. La selección conserva `TRIP_ID` como `source_id`.
