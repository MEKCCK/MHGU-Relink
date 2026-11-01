# MHGU para Android

Juega tu propia copia de Monster Hunter Generations Ultimate en Android, de forma nativa y con controles táctiles pensados
para teléfonos. No es un emulador: el código del juego se traduce para correr directamente en tu dispositivo.

**Este proyecto no incluye código del juego, archivos del juego, ROMs ni keys. Necesitas tu propia copia del juego.**

Hecho con Claude Code (Opus 5.5). Muchas de las decisiones fueron propias, porque la IA no está lista para tener la última palabra en algo así.

[Read this guide in English](README.md)

## Qué ofrece

- Hasta 120 fps en teléfonos potentes
- Llena toda la pantalla, desde teléfonos ultra anchos hasta consolas portátiles 4:3
- Controles táctiles: pad de movimiento, arrastrar para girar la cámara, deslizar la barra de objetos, tocar las Hunter Arts y tocar el ícono del objetivo para fijar la cámara
- Tres disposiciones de los botones de ataque: píldora, botones separados o una banda en la esquina de la pantalla
- Mantén apretado el engranaje para mover y cambiar el tamaño de cada control
- También funciona con joystick: los controles táctiles se ocultan mientras haya uno conectado. Aprieta **+** y **-** juntos para abrir las opciones
- Vibración y un contador opcional de FPS y temperatura

## ¿Qué tan bien corre?

Medido con el contador en pantalla:

| Dispositivo | Chip | RAM | Resultado |
|---|---|---|---|
| Galaxy S25 Ultra | Snapdragon 8 Elite | 12 GB | 120 fps |
| Anbernic RG406V | Unisoc T820 | 8 GB | 48 fps a resolución completa, 60 fps al 50 % |
| moto g06 power | MediaTek MT6769 | 4 GB | 16 fps a resolución completa, hasta 28 fps al 50 % |

En un teléfono con 4 GB de RAM, cierra las otras apps antes de jugar: Android puede cerrar el juego si se queda sin memoria.
Para que vaya más rápido, entra al engranaje, **Pantalla**, baja la **Resolución 3D** y reinicia el juego.

## Qué necesitas

- **Un dispositivo Android:** Android 10 o superior, de 64 bits, con Vulkan 1.2. Casi cualquiera de los últimos años.
- **Unos 13 GB libres** en el dispositivo.
- **Una PC con Windows o Linux,** solo para la instalación, con unos 10 GB libres.
- **Tu propia copia del juego,** actualizada a la versión 1.4.0.

## Instalación

Son tres pasos, y se hacen una sola vez.

### Paso 1: Consigue los archivos de tu juego

Necesitas dos carpetas de tu propio juego: `exefs` y `romfs`. Deben incluir la actualización 1.4.0.

Si tu copia está en el emulador Ryujinx en tu PC:

1. Asegúrate de que el juego tenga aplicada la actualización 1.4.0.
2. Haz clic derecho en el juego, entra a **Extract Data** y extrae **ExeFS** y después **RomFS** en una carpeta vacía.

Deja las dos en una misma carpeta, por ejemplo `C:\MHGU`:

```
MHGU
├─ exefs
└─ romfs
```

Este proyecto no explica cómo volcar un juego de tu consola y no ofrece archivos del juego, keys ni enlaces a ellos.

### Paso 2: Crea el código del juego

La app no trae código del juego, así que creas el tuyo a partir de tu copia. Se hace en tu PC y no envía nada a ningún lado.

1. Instala [Python](https://www.python.org/downloads/). En Windows, marca **Add python.exe to PATH** en el instalador.
2. En esta página, haz clic en el botón verde **Code**, luego en **Download ZIP**, y descomprímelo.
3. Abre la carpeta descomprimida y entra a `tools`.
4. Windows: haz clic en la barra de direcciones de esa ventana, escribe `cmd` y presiona Enter. Linux: abre una terminal en esa carpeta.
5. Escribe esto, con tu propia carpeta, y presiona Enter:

   ```
   python build_game.py C:\MHGU
   ```

La primera vez descarga el compilador de Android de Google (unos 1,8 GB). Después trabaja solo: unos 6 minutos en una PC rápida,
más en una lenta. Cuando diga **Done**, tu carpeta tendrá dos archivos nuevos:

```
MHGU
├─ exefs
├─ romfs
├─ libmhgugame.so
└─ libmhgugame.id
```

**Estos dos archivos salen de tu juego: guárdalos para ti y no los compartas.**

### Paso 3: Instala

1. Copia la carpeta `MHGU` completa a tu dispositivo, por ejemplo con un cable USB. Puede ir en cualquier lugar de la memoria o de
   una tarjeta SD, menos dentro de `Android/data`.
2. En tu dispositivo, descarga el APK desde [Releases](../../releases/latest) y ábrelo. Android puede pedirte permitir la instalación
   de apps desde el navegador.
3. Abre **MHGU**, toca **Dar permiso y elegir carpeta** y activa el acceso a los archivos cuando Android lo pida. El juego solo lee esa carpeta.
4. Busca tu carpeta `MHGU` y toca **Usar esta carpeta**.

La app revisa la carpeta, te dice exactamente qué falta si algo está mal y arranca el juego.

Tus partidas y ajustes se guardan aparte, en `Android/data/com.mhgu.recomp/files`. Para elegir otra carpeta después: engranaje,
**Opciones del menú**, **Carpeta del juego**.

## Samsung: Game Booster

El panel de rendimiento de Samsung solo aparece en apps que reconoce como juegos, y un APK instalado a mano no cuenta. Para
tenerlo, instala el APK una vez desde una PC con [adb](https://developer.android.com/tools/adb):

```
adb install -r -i com.mhgu.recomp MHGU-0.1.0.apk
```

Es opcional: el juego corre igual sin esto. Para al menos verlo en Gaming Hub: **Mis juegos**, **Biblioteca**, los tres
puntos, **Añadir juegos**.

## Si algo falla

| Problema | Qué hacer |
|---|---|
| `python` no se reconoce | Instala Python de nuevo, marca **Add python.exe to PATH** y abre otra terminal |
| "there is no exefs/main" | La ruta debe ser la carpeta que contiene `exefs` y `romfs` |
| "Este exefs no es de la versión 1.4.0" | Usa el `exefs` con la actualización 1.4.0 |
| "romfs está incompleto" | Usa el `romfs` del juego base con la actualización aplicada. Tiene 35.531 archivos |
| "Falta libmhgugame.so" | Haz el paso 2 y copia los dos archivos nuevos a la carpeta de tu dispositivo |
| "Este libmhgugame.so es de otra versión" | Una versión nueva de la app lo necesita otra vez: repite el paso 2 con la nueva descarga |
| El juego se cierra solo | Cierra otras apps y baja la Resolución 3D. Suele ser falta de memoria |

Si sigue fallando, abre un [issue](../../issues) con el modelo de tu dispositivo y el archivo
`Android/data/com.mhgu.recomp/files/log.txt`. No adjuntes archivos del juego.

## Legal

Este proyecto no está afiliado, respaldado ni patrocinado por la Gran C ni la Gran N. Monster Hunter es una marca de la Gran C.
Switch es una marca de la Gran N.

Este repositorio y sus lanzamientos no contienen código del juego, datos del juego, ROMs ni keys. Úsalo solo con una copia del
juego que te pertenezca. Los pedidos de archivos del juego, keys o `libmhgugame.so` ya hechos se cerrarán.

Titulares de derechos: abran un issue para ponerse en contacto.

## Créditos

Basado en el proyecto de código abierto MHGURecomp, con [licencia MIT](LICENSE), y las bibliotecas listadas en
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES). Sus licencias también están en la app: engranaje, **Opciones del menú**, **Licencias**.

## Para desarrolladores

Mira [CLAUDE.md](CLAUDE.md) para ver cómo se organiza, se compila y se prueba el proyecto, con notas de instalación paso a paso.
