# Smash Mod Hub

Gestor **y** descargador de mods para **Super Smash Bros. Ultimate** en Switch con Atmosphere.
Es un homebrew `.nro` que se abre desde el Homebrew Menu, se conecta a **GameBanana**, descarga
los mods con su miniatura y los instala en `sd:/ultimate/mods` para que los cargue **ARCropolis**.

> Hecho para uso con ARCropolis + Skyline ya instalados. No instala plugins ni toca tu `atmosphere/`.

## Funciones

| | |
|---|---|
| **Explorar GameBanana** | Cuadrícula con miniaturas, orden (destacados / nuevos / actualizados), búsqueda con el teclado del sistema, paginación |
| **Filtros por categoría** *(nuevo)* | Los mismos filtros de GameBanana: categorías (Skins, Efectos, Sonidos, Final Smash...) y dentro de cada una sus subcategorías (cada personaje). Se combinan con el orden y la búsqueda |
| **Reslotter** *(nuevo)* | Cambia el slot (c00–c07) de un mod de personaje renombrando sus archivos: modelos, animaciones, cámara, efectos, sonidos y retratos. Con vista previa, aviso de choques con otros mods y deshacer automático si algo falla |
| **Ficha del mod** | Galería de capturas, descripción, autor, categoría, fechas, descargas/likes, lista de archivos con tamaño |
| **Descarga + instalación** | Cola de descargas con progreso, **velocidad y tiempo restante**, descarga en **varias conexiones a la vez**, verificación **MD5**, extracción de `.zip/.7z/.rar`, detección automática de la carpeta del mod |
| **Instalados con imagen** | Cada mod guarda su miniatura (o usa su `preview.webp`); activar/desactivar con un botón, borrar, tamaño en disco |
| **Activar/desactivar sin tocar ARCropolis** | Los mods desactivados se mueven a `sd:/ultimate/mods_disabled` (ARCropolis no los ve) |
| **Actualizaciones** | Detecta cuándo hay un archivo más nuevo en GameBanana y reinstala en la misma carpeta |
| **Perfiles** | Guarda el conjunto de mods activos ("Online", "Torneo", "Skins"...) y cámbialo en un paso |
| **Conflictos** | Busca archivos repetidos entre los mods activados (lo que hace que ARCropolis los ignore) |
| **Importar** | Copia `.zip/.7z/.rar` a `sd:/switch/smash-mod-hub/import` y se instalan desde Herramientas |
| **Seguridad al extraer** | Se descartan rutas con `..`, se limpian nombres inválidos para FAT32 y no se instalan plugins/`atmosphere/` de los paquetes |
| **Diagnóstico** | Comprueba ARCropolis/Skyline, espacio libre, TLS y muestra el registro |
| **Español / English** | Cambiable en Herramientas |

## Controles

| Botón | Acción |
|---|---|
| `L` / `R` | Cambiar de pestaña |
| Cruceta / stick izq. | Moverse |
| `A` | Abrir / activar-desactivar / confirmar |
| `B` | Volver / cancelar · **Filtros por categoría** (Explorar) · **Reslotear** (Instalados) |
| `Y` | Buscar (Explorar) · Actualizar o ver (Instalados) · Nuevo perfil · Limpiar terminadas |
| `X` | Orden (Explorar) · Borrar (Instalados) · Cancelar/quitar (Descargas) |
| `ZL` / `ZR` | Página anterior/siguiente (Explorar) · Desactivar/activar todos (Instalados) |
| `-` | Volver al inicio, quitar búsqueda y filtro, y refrescar (Explorar) · Buscar actualizaciones (Instalados) |
| Stick der. | Desplazar la descripción |
| `+` | Salir |

> Ábrelo con **título secuestrado** (mantén `R` al pulsar un juego en el menú de homebrew) para tener toda la RAM;
> desde el álbum puede quedarse corto con listas largas de imágenes.

## Filtros por categoría

En **Explorar** pulsa `B`:

1. Aparece la lista de categorías del juego (la misma estructura que en GameBanana). `A` abre una categoría y muestra
   sus subcategorías (por ejemplo *Skins → Mario*). La primera fila ("Todo en ...") filtra por la categoría entera.
2. `X` filtra ya por la categoría marcada sin abrirla. `Y` busca por texto dentro de la lista (útil con ~90 personajes).
   `ZL`/`ZR` saltan de 14 en 14. `B` sube un nivel.
3. El filtro activo se ve arriba ("Filtro: Skins / Mario"). Se combina con el orden (`X`) y con la búsqueda (`Y`):
   al buscar dentro de una categoría la app filtra en local los primeros 400 mods de esa categoría (el servidor de
   GameBanana no combina categoría y texto). `-` quita todo.
- Las categorías se guardan en la SD 24 h (`categories.json`) y se usan sin conexión; si la API de categorías no
  responde, se usan las que ya aparecieron en los mods cargados (lista parcial, avisado en pantalla).

## Reslotter

En **Instalados**, selecciona un mod y pulsa `B` (o *Herramientas → Reslottear un mod*):

1. La app lee los nombres del mod (no el contenido), detecta el personaje y los slots que usa (`c00`...`c07`).
2. Elige el slot nuevo de cada uno con izquierda/derecha. Si otro mod activado ya usa ese personaje y slot, te avisa.
3. `A` → confirmar. Se renombran, en dos fases, las carpetas y archivos con el slot en el nombre:
   `fighter/<x>/model|motion/**/cNN`, `camera/fighter/<x>/cNN`, `effect/fighter/<x>/**` (`ef_<x>_cNN.eff`, `trail_cNN`,
   `<efecto>_cNN`), `sound/bank/fighter|fighter_voice/se_|vc_<x>_cNN.*`, `ui/replace(_patch)/chara/**/chara_N_<x>_NN.bntx` y
   las carpetas `copy_<personaje>_*` de Kirby.
4. **Seguridad:** antes de empezar se anota el plan en un diario; si un renombrado falla o cancelas con `B`, se deshace
   todo en orden inverso; si la consola se apaga a mitad, al abrir la app se termina el trabajo pendiente.
- **No se tocan** mods con `config.json` que mencione slots (slots extra, archivos compartidos) ni mods en `c08` o superior:
  necesitan configuración/plugins y la app lo avisa. Los archivos de `item/`, `stage/` y demás no se modifican.
- Después de reslotear, reinicia el juego. Para volver atrás, reslotea de nuevo al slot original.

## Estabilidad (qué se hace para no cerrarse ni congelarse)

- **Excepciones controladas:** un error inesperado (sin memoria, JSON raro) en un hilo de fondo, en una descarga o en un
  fotograma se anota en el registro y la app sigue; solo si se repite 30 veces seguidas se cierra de forma limpia.
- **Tareas que nunca se quedan "en curso" para siempre**, aunque el sistema no pueda crear un hilo.
- **Suspensión de la consola:** al despertar se cierran las conexiones guardadas (los sockets viejos están muertos).
- **Disco:** se comprueba el espacio libre antes de descargar y, al extraer, que lo descomprimido quepa (protege de
  archivos "bomba"); el reslot es atómico (ver arriba).
- **Memoria acotada:** ventana de descarga ≤ 16 MB, tope de 16 MB por respuesta JSON, cachés con límite.
- **Diagnóstico de congelaciones:** si un fotograma tarda más de 150 ms se anota en `log.txt` ("fotograma lento ...")
  con la pestaña y las descargas activas; si algo se congela, envía ese registro.

## Rendimiento y velocidad de descarga

- **Descargas:** si el servidor admite `Range`, cada archivo se baja en trozos (de 256 KB a 2 MB según su tamaño) por
  4 conexiones a la vez (se escriben en orden en la SD). El **MD5 se calcula mientras se descarga** (ya no hay que
  releer el archivo entero), si la red se corta se **reanuda desde donde iba** (hasta 3 veces) y solo si no se puede
  vuelve a una conexión. Mientras hay descargas o instalaciones, la CPU se sube de velocidad (descomprimir es trabajo
  de CPU) y se baja al terminar; la extracción usa bloques de lectura/escritura grandes. El número de conexiones se cambia en
  *Herramientas → Conexiones por descarga* (1, 2, 3, 4 o 6). Si un servidor va peor con varias, pon 1.
- **Interfaz fluida:** las tareas pesadas (red, JSON, imágenes, instalación) corren en hilos de fondo con menos
  prioridad que la interfaz y en otro núcleo de la CPU cuando hay uno libre; el texto se dibuja repartido entre
  frames, las páginas ya vistas se guardan (ir atrás es instantáneo) y se puede seguir moviendo la cruceta mientras
  carga la página siguiente.
- **Red:** las conexiones HTTPS se reutilizan (antes cada imagen abría una nueva) y la app pide a la consola más
  sesiones de red y búferes TCP más grandes; si el sistema los rechaza, usa los de por defecto.
- Si algo va lento, *Herramientas → Diagnóstico* muestra el registro con la sonda de la descarga
  (`DL sonda ... rangos=si/no`).

## Instalar

1. Compila el `.nro` (abajo) o descarga el artefacto del workflow de GitHub Actions.
2. Copia `smash-mod-hub.nro` a `sd:/switch/smash-mod-hub/`.
3. Ábrelo desde el Homebrew Menu. Crea solo las carpetas que necesita.

## Compilar

### Opción A: GitHub Actions (sin instalar nada)
Sube este proyecto a un repositorio de GitHub. El workflow `.github/workflows/build.yml` ejecuta los tests,
instala las librerías de devkitPro, descarga `cacert.pem` y publica el `.nro` como artefacto.

### Opción B: en tu PC con devkitPro
```sh
dkp-pacman -S switch-dev switch-portlibs
curl -o romfs/cacert.pem https://curl.se/ca/cacert.pem   # HTTPS verificado
make
```
Si el enlazado falla por una librería que falta, revisa `PC_LIBS` en el `Makefile` (las dependencias las
toma de `pkg-config`; la lista manual es solo un plan B y puede variar entre versiones de portlibs).

### Tests del núcleo (JSON, GameBanana, instalador, perfiles, conflictos, rutas seguras)
```sh
sh tests/run.sh      # solo necesita g++; no hace falta devkitPro
```

## Cómo se organiza en la SD

```
sd:/ultimate/mods/               mods activados (ARCropolis los carga)
sd:/ultimate/mods_disabled/      mods desactivados por Smash Mod Hub
sd:/switch/smash-mod-hub/
    mods.json                    metadatos (id de GameBanana, miniatura, archivo instalado, perfiles)
    settings.json                ajustes
    thumbs/ cache/ downloads/    miniaturas, caché de imágenes, archivos descargados
    import/                      deja aquí tus .zip/.7z/.rar para instalarlos
    log.txt                      registro (se recorta solo)
```

## Compatibilidad con el firmware 23.0.0

- La app es un homebrew normal (`.nro`) y solo usa servicios estándar de libnx (mando, teclado del sistema, fuente
  compartida, sockets y tarjeta SD). El workflow actualiza devkitPro/libnx a la última versión **antes** de compilar,
  así que el `.nro` sale con el soporte más reciente que publique devkitPro.
- Lo que sí tiene que estar al día en tu consola es **Atmosphere**: el soporte de la 23.0.0 llegó con la **1.12.0**,
  y la **1.12.1** corrigió fallos posteriores (applets del navegador, qlaunch). Usa la 1.12.1 o más reciente
  (y actualiza fusee/hekate como indican sus notas).
- **ARCropolis y Skyline** también deben ser versiones que funcionen con tu versión del juego y de Atmosphere; esta app
  solo coloca los mods, no los carga. Si el juego no carga mods, el problema está en esa parte, no en Smash Mod Hub.
- *Herramientas → Diagnóstico* muestra el firmware detectado y si encuentra ARCropolis/Skyline.
- No he podido probarlo en una consola con 23.0.0: la compatibilidad se basa en la documentación de cambios del
  firmware y en que libnx se mantiene al día.

## Limitaciones conocidas

- **ID del juego:** usa `6498` (Super Smash Bros. Ultimate en GameBanana). Si no aparecen resultados, se puede
  cambiar en *Herramientas → ID del juego*.
- **Actualizaciones:** se comparan los ID de archivo de la página del mod. Si un mod tiene varios archivos (variantes)
  y instalaste uno que no es el más reciente, puede marcarlo como "actualizar" aunque no lo sea.
- **Categorías y filtros:** los endpoints de categorías de GameBanana no se pudieron probar en vivo; la app prueba varias
  variantes y recuerda la que funciona, y si ninguna responde usa las categorías ya vistas. El orden "Nuevos/Actualizados"
  dentro de una categoría usa los valores de orden de `Mod/Index`; si el servidor los rechaza se muestra sin ordenar.
- **Reslotter:** no cambia el contenido de los archivos (p. ej. nombres internos de bancos de sonido `.nus3bank`);
  cubre lo que ARCropolis resuelve por ruta. Los mods con `config.json` de slots no se tocan.
- **Conflictos:** compara rutas dentro de cada mod (sin distinguir mayúsculas). No analiza el contenido.
- Mods que necesitan **plugins** (`.nro` de Skyline) o parches de `exefs` se instalan a mano: la app los ignora a propósito.
- Los mods que dan ventaja online pueden acarrear baneos; usa perfiles para quitarlos antes de jugar online.

## Estado de la verificación

- El núcleo (JSON, MD5, API de GameBanana con respuestas de ejemplo, categorías y filtros, **reslotter** —incluida la
  simulación de fallos y apagones en *cada* paso del plan—, instalador, mods/perfiles/conflictos, rutas seguras)
  está cubierto por `tests/` y pasa con ASan/UBSan.
- La descarga por trozos (orden, reintentos, cancelación, disco lleno, servidor sin `Range`) y la lógica de texto de la
  interfaz (ajuste de línea, reparto por frames, caché) también tienen tests (`tests/run.sh`), con detector de errores
  de memoria y de carreras entre hilos.
- La interfaz SDL2, el mando/teclado de libnx y el enlazado final **solo se han comprobado sintácticamente** contra
  cabeceras de prueba; las velocidades reales dependen de tu red y del servidor, y no se han podido medir aquí.
- Los endpoints de GameBanana (API v11) se tomaron de documentación y proyectos de terceros; no pudieron probarse
  en vivo desde el entorno donde se escribió el código. Si algún endpoint cambia, el registro (`log.txt`) muestra la URL y el error.

## Aviso

Proyecto de la comunidad, sin relación con Nintendo, GameBanana ni el equipo de ARCropolis. Respeta los términos de
GameBanana y las licencias/créditos de cada mod. Usa mods bajo tu responsabilidad.
