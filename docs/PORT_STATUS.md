# Estado de la conversión Java a C++

## Implementado de forma funcional

| Área del repositorio Java | Implementación C++ |
|---|---|
| modelos de trayectoria, ubicación y píxel | `types.hpp` |
| carga Porto/Shenzhen (`DM`) | `io.cpp` |
| proyección y preprocesamiento (`PreProcess`) | `projection.cpp` |
| heap diferido y VQGS/VQGS+ (`TrajHeap`, `VFGS`) | `vqgs.cpp` |
| calidad multi-zoom (`Quality`) | CLI y `quality_at_projection` |
| representatividad (`VFGSColor`) | `representative_scores` |
| quad-tree | `quadtree.cpp` |
| listas locales y estructura InvQuad | `invquad.cpp` |
| comparación Full/VQGS+/Color/Random | `cheetahtraj-viewer.exe` |
| exportadores y evaluaciones por lotes | opciones y CSV del CLI |

## Reemplazado por equivalentes C++

- Processing, JavaFX y Unfolding Maps se sustituyen por un visor Win32/GDI.
- Los sketches separados de prueba y perfilado se consolidan en el CLI, sus
  opciones reproducibles y las pruebas automatizadas.
- La serialización Java se sustituye por CSV legible y portable.

## No trasladado intencionalmente

- envío de correos al finalizar experimentos;
- ventanas Swing auxiliares sin lógica algorítmica;
- rutas absolutas y configuraciones locales de los autores;
- código comentado, duplicado o experimental que no participa en el flujo de
  CheetahTraj.

Estas exclusiones no alteran el algoritmo, el índice, las métricas ni las
comparaciones visuales. La futura extensión temporal de la tesis no pertenece
al repositorio Java original y debe añadirse como un módulo nuevo.
