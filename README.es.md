# MHGU para Android

Juega tu propia copia de Monster Hunter Generations Ultimate en Android, de forma nativa y con controles táctiles pensados
para teléfonos. No es un emulador: el código del juego se traduce para correr directamente en tu dispositivo.

**Este proyecto no incluye código del juego, archivos del juego, ROMs ni keys. Necesitas tu propia copia del juego.**

Hecho con Claude Code (Opus 5.5). Muchas de las decisiones fueron propias, porque la IA no está lista para tener la última palabra en algo así.

[Read this guide in English](README.md)

## Qué ofrece

### Cacería con controles táctiles

- **Movimiento:** un pad fijo, que no se mueve con el dedo. Arrastra más allá de su borde para correr; con el arma desenfundada eso se desactiva, para que no salgas corriendo en plena pelea
- **Cámara:** arrastra en cualquier parte de la pantalla para girarla, como en un juego de teléfono. Con un doble toque se centra
- **Fijar al monstruo:** toca el ícono del objetivo y la cámara lo sigue hasta que lo vuelvas a tocar, como en los Monster Hunter más nuevos
- **Objetos:** la barra queda abajo al centro. Tócala para usar el objeto y deslízala para cambiarlo
- **Hunter Arts:** toca el panel de cada una para usarla
- **Ataques:** tres disposiciones a elegir: píldora dividida en diagonal, botones separados o una banda en U en la esquina de la pantalla
- **Otros botones:** esquivar, cubrirse y enfundar el arma, cada uno con su ícono

### Menús y comodidad

- El menú de pausa, la bolsa y las tiendas se manejan con una cruceta y botones en pantalla
- El teclado del teléfono sirve para escribir el nombre de tu personaje
- El menú de opciones se usa con el dedo y tiene el estilo visual del juego
- **Beber y comer más rápido:** dos trucos opcionales, apagados por defecto, en el engranaje, **Trucos**. Aceleran la animación x1,7 o x2,5, pero no permiten caminar mientras bebes
- Importa tu partida desde un emulador o desde otro dispositivo
- El juego se pausa solo cuando sales de la app
- Vibración al tocar los botones y vibración del juego en el teléfono, con o sin joystick

### Personalización

- Mantén apretado el engranaje para abrir el editor: mueve y cambia el tamaño de cada botón, y mueve la barra de objetos, las Hunter Arts y el ícono del objetivo
- La interfaz del juego se reacomoda para no quedar bajo los pulgares. Se puede desactivar
- Opacidad de los controles y velocidad de la cámara ajustables
- Los botones ZL y ZR vienen ocultos: se activan en el engranaje, **Controles**

### Pantalla y rendimiento

- Hasta 120 fps en teléfonos potentes, con límite de FPS configurable
- Llena toda la pantalla, desde teléfonos ultra anchos hasta consolas portátiles 4:3
- Resolución 3D ajustable para ganar rendimiento en equipos modestos
- Contador opcional de FPS y temperatura
- Usa menos memoria en dispositivos con GPU Mali y en teléfonos con poca RAM

### Con joystick

- Los controles táctiles se ocultan apenas conectas uno y vuelven cuando lo desconectas, sin reiniciar
- Aprieta **+** y **-** juntos para abrir las opciones
- El clic del stick derecho fija al monstruo

### Heredado de la versión de PC

Red local, mods, vida de los monstruos ajustable y misiones de la aldea en el Hub también están en las opciones, pero todavía no se probaron a fondo en Android.

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

- **Un dispositivo Android:** Android 10 o superior, de 64 bits, con Vulkan 1.2. Casi cualquiera de los últimos años. Con Vulkan 1.1 funciona de forma experimental.
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

## Importar tu partida

Puedes traer tu partida desde un emulador o desde otro dispositivo:

1. Busca el archivo `system` de tu partida. En Ryujinx: clic derecho en el juego, **Open User Save Directory**. En yuzu: clic derecho
   en el juego, **Open Save Data Location**.
2. Copia ese archivo a tu dispositivo, por ejemplo a Descargas.
3. En el juego, entra al engranaje, **Opciones del menú**, **Importar partida**, y elige el archivo.

Se reemplazan todos los personajes del dispositivo y el juego se reinicia. Tu partida anterior queda guardada en
`Android/data/com.mhgu.recomp/files/MHGURecomp/save_backups`.

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
