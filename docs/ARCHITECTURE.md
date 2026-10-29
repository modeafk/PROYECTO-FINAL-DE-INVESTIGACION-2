# Arquitectura de la conversión

## Flujo principal

```text
dataset GPS
    |
    v
load_dataset
    |
    v
rasterize_dataset (Web Mercator)
    |
    v
VqgsEngine::select
    |-- max-heap diferido
    |-- ganancia marginal
    |-- tolerancia delta
    v
VqgsResult
    |-- IDs seleccionados
    |-- cobertura aumentada
    |-- ganancias marginales
    |
    +--> representative_scores
    +--> quality
    +--> archivos CSV
```

## Preprocesamiento

`load_dataset` interpreta el formato utilizado por los archivos de Porto y
Shenzhen. Cada línea corresponde a una trayectoria y contiene pares de
longitud y latitud. `rasterize_dataset` proyecta las coordenadas con Web
Mercator y elimina los píxeles duplicados dentro de cada trayectoria.

## VQGS+

`VqgsEngine::select` reproduce el núcleo de `vqgs.vfgs.VFGS`. Cada trayectoria
comienza con el número total de píxeles como cota superior. Al llegar a la cima
del heap se recalcula su ganancia respecto de la cobertura actual. Si la cota
de las demás trayectorias no puede superarla, se selecciona; de lo contrario,
se reinserta con su ganancia actualizada.

Al seleccionar una trayectoria se agrega su vecindad de radio `delta` a la
cobertura, limitada a píxeles existentes en el dataset. Esto evita que una
trayectoria cercana a otra ya elegida sea tratada como información nueva.

## Calidad y representatividad

`quality` calcula la fracción de píxeles de la visualización completa cubiertos
por la muestra. `representative_scores` asigna cada trayectoria del dataset al
representante seleccionado con mayor superposición, para permitir la
codificación por color de VQGS+.

## Índice espacial

`QuadTreeIndex` organiza las trayectorias por regiones geográficas.
`InvQuadIndex` construye para cada nodo una lista invertida ordenada por la
selección VQGS+ local y almacena su calidad acumulada. Una consulta desciende
por los nodos que intersectan la región solicitada y recupera elementos hasta
alcanzar la calidad objetivo. El índice permanece separado del filtro temporal,
lo que permite incorporar ventanas sin acoplar tiempo y espacio.

## Visualización

`cheetahtraj-viewer.exe` sustituye los sketches Processing por una aplicación
nativa Win32/GDI. Las cuatro vistas permiten comparar el dataset completo,
VQGS+, la codificación de representatividad y un baseline aleatorio. La tasa y
la tolerancia pueden recalcularse durante la exploración.

## Punto de extensión temporal

La extensión de tesis puede insertarse entre la consulta del índice y
`VqgsEngine::select`:

```text
consulta espacial -> filtro temporal -> VQGS+ -> visualización
```

El dataset oficial incluido no contiene marcas temporales por punto, por lo que
estas deberán reconstruirse desde otro campo del dataset original o añadirse al
modelo `Location` cuando se incorpore la extensión temporal.
