# Índice espacial-temporal en terminal

`cheetahtraj-spatiotemporal.exe` ejecuta la extensión temporal de InvQuad en
dos fases separadas. El programa anterior, `cheetahtraj-temporal.exe`, sigue
disponible como baseline que aplica VQGS+ independientemente por ventana.

## Uso interactivo

Después de preparar el índice una vez, ejecute desde la carpeta del proyecto:

```powershell
.\cheetahtraj-spatiotemporal.exe
```

El programa muestra las ventanas y sus horarios **UTC**. Escriba el número
de una ventana, indique la calidad objetivo (o pulse Enter para `0.90`) y
escriba cuatro coordenadas para consultar una región. Si deja vacía la región,
se utiliza la extensión espacial completa de esa ventana. Al terminar la
consulta, vuelve a mostrar las ventanas; `q` sale del programa. La vista
interactiva muestra los primeros diez `TRIP_ID`; el comando `query` imprime
todos los identificadores.

Sin argumentos, busca primero `output/temporal_invquad`. Si ese índice no
existe, usa el ejemplo `output/porto_st_index_1k` preparado con los primeros
1000 viajes de Porto. Para elegir otro índice:

```powershell
.\cheetahtraj-spatiotemporal.exe interactive --index-dir output\mi_indice
```

UTC no equivale siempre a la hora local de Porto: en horario de verano Porto
está una hora adelantado respecto de UTC.

## Construcción offline

```powershell
.\build-gcc.ps1
.\cheetahtraj-spatiotemporal.exe build `
  --format porto `
  --dataset "C:\Users\User\Documents\UCSP\2026-01\PFC1\Tema_investigacion\IMPLEMENTACION\implementacion1\archive\train.csv" `
  --limit 1000 `
  --window-size 3600 `
  --window-step 3600 `
  --height 2 `
  --delta 4 `
  --zoom 15 `
  --index-dir output\porto_st_index
```

La carga Porto reconstruye el tiempo de cada punto usando el `TIMESTAMP`
inicial y un intervalo de 15 segundos. Una ventana es semiabierta `[inicio,
fin)`. Se conservan únicamente los puntos de esa ventana; con `--min-points 2`
se excluyen fragmentos con menos de dos puntos. El índice guarda un archivo
de datos y otro de InvQuad para cada ventana no vacía. `manifest.csv` enumera
los horarios, los parámetros visuales y la cantidad de segmentos. Las
calidades acumuladas de cada nodo se calculan exclusivamente a partir de los
fragmentos de su ventana.

Para el CSV genérico se utiliza `--format generic` y las columnas
`trajectory_id,longitude,latitude,timestamp`.

## Consulta online

```powershell
.\cheetahtraj-spatiotemporal.exe query `
  --index-dir output\porto_st_index `
  --window 0 `
  --bbox 41.10 41.20 -8.70 -8.50 `
  --quality-target 0.80
```

`--bbox` recibe latitud mínima, latitud máxima, longitud mínima y longitud
máxima. Se puede usar `--at 1372640500` en lugar de `--window 1`; si las
ventanas se solapan, `--at` toma la primera que contiene ese instante. Los
identificadores de ventana y límites están en `manifest.csv`.

La consulta carga solo el índice y los puntos de la ventana elegida. Primero
recupera prefijos InvQuad para la región y después calcula la calidad real
contra **todos los puntos de esa misma ventana y región**. Si la región corta
un nodo y el prefijo recuperado queda por debajo de `quality-target`, se
agregan trayectorias mediante VQGS+ hasta alcanzar el umbral. La salida
distingue las trayectorias del índice de las agregadas, muestra la calidad
medida y el tiempo online, e imprime los `TRIP_ID` seleccionados.

El tiempo de consulta incluye lectura desde disco y verificación de calidad.
La complementación puede aumentar ese tiempo en consultas difíciles. El
prototipo rasteriza puntos GPS como el port C++ existente; todavía no dibuja
segmentos continuos entre puntos ni dispone de interfaz gráfica. El índice
guardado depende del zoom, centro de proyección, delta y altura utilizados
durante la construcción. Para cambiar esos parámetros hay que reconstruirlo.
