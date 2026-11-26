# Changelog

Lodestar (PS4Mount PS4 before 2.0.0) began as a PS4-only port of [PS4Mount](https://github.com/gasb07/ps4mount) by gasb07 (PS5); its versions start again at 0.1.0. The history of the original is in its project and in this repository's git history.

## 3.1.0-rc.2

**Test release with the native PS4 app.** Browse and search your library with the DualShock 4, open game details, adjust settings, and import files from a URL you supply. The separate web panel works on mobile and desktop and supports file uploads and transfer management.

1. Load `lodestar-ps4.bin` with GoldHEN. Use the `.elf` if your loader takes ELF.
2. Install `lodestar-panel.pkg` through GoldHEN’s Package Installer. If you have the previous Lodestar tile, install this PKG to update it.
3. Open **Lodestar** on the PS4, or use the address in the startup notification to open the web panel on a trusted local network.

Package transfers and URL imports now use consistent labels in the app, web panel and documentation. Host tests and package validation pass. Startup, controller and keyboard on actual PS4 hardware still need verification. [Track PS4 testing](https://github.com/felipebasurto/Lodestar-PS4/issues/13).

The README includes measured HDD performance and guidance on USB storage. The read-only `tools/exfatmap.py` diagnostic handles aligned reads from raw macOS devices. [Known issues and improvements](https://github.com/felipebasurto/Lodestar-PS4/issues).

**Español**

**Versión de prueba con app nativa de PS4.** Usa el DualShock 4 para recorrer y buscar en la biblioteca, abrir fichas, cambiar ajustes y importar archivos desde una URL que indiques. El panel web sigue disponible en móvil y escritorio para subir archivos y gestionar transferencias.

1. Carga `lodestar-ps4.bin` con GoldHEN. Usa el `.elf` si tu cargador trabaja con ELF.
2. Instala `lodestar-panel.pkg` con el instalador de paquetes de GoldHEN. Si ya tienes el icono anterior, instala este PKG para actualizarlo.
3. Abre **Lodestar** en la PS4 o usa la dirección del aviso de arranque para abrir el panel web desde una red local de confianza.

La app, el panel web y la documentación usan los mismos términos para transferir paquetes e importar archivos. Las pruebas de host y la validación del paquete pasan. El arranque, el mando y el teclado siguen pendientes de validación en una PS4 real. [Pruebas en consola](https://github.com/felipebasurto/Lodestar-PS4/issues/13).

El README recoge las mediciones del HDD y explica cómo afecta la unidad USB al rendimiento. El diagnóstico de solo lectura `tools/exfatmap.py` admite lecturas alineadas de dispositivos crudos en macOS. [Problemas conocidos y mejoras](https://github.com/felipebasurto/Lodestar-PS4/issues).

## 3.1.0-rc.1

**First test release with the native PS4 app.** Browse and search your library with the DualShock 4, open game details, adjust settings, and import files from a URL you supply. The separate web panel works on mobile and desktop and supports file uploads and transfer management.

1. Load `lodestar-ps4.bin` with GoldHEN. Use the `.elf` if your loader takes ELF.
2. Install `lodestar-panel.pkg` through GoldHEN’s Package Installer. If you have the previous Lodestar tile, install this PKG to update it.
3. Open **Lodestar** on the PS4, or use the address in the startup notification to open the web panel on a trusted local network.

Host tests and package validation pass. Startup, controller and keyboard on actual PS4 hardware still need verification. [Track PS4 testing](https://github.com/felipebasurto/Lodestar-PS4/issues/13).

The README includes measured HDD performance and guidance on USB storage. The read-only `tools/exfatmap.py` diagnostic handles aligned reads from raw macOS devices. [Known issues and improvements](https://github.com/felipebasurto/Lodestar-PS4/issues).

**Español**

**Primera versión de prueba con app nativa de PS4.** Usa el DualShock 4 para recorrer y buscar en la biblioteca, abrir fichas, cambiar ajustes y importar archivos desde una URL que indiques. El panel web sigue disponible en móvil y escritorio para subir archivos y gestionar transferencias.

1. Carga `lodestar-ps4.bin` con GoldHEN. Usa el `.elf` si tu cargador trabaja con ELF.
2. Instala `lodestar-panel.pkg` con el instalador de paquetes de GoldHEN. Si ya tienes el icono anterior, instala este PKG para actualizarlo.
3. Abre **Lodestar** en la PS4 o usa la dirección del aviso de arranque para abrir el panel web desde una red local de confianza.

Las pruebas de host y la validación del paquete pasan. El arranque, el mando y el teclado siguen pendientes de validación en una PS4 real. [Pruebas en consola](https://github.com/felipebasurto/Lodestar-PS4/issues/13).

El README recoge las mediciones del HDD y explica cómo afecta la unidad USB al rendimiento. El diagnóstico de solo lectura `tools/exfatmap.py` admite lecturas alineadas de dispositivos crudos en macOS. [Problemas conocidos y mejoras](https://github.com/felipebasurto/Lodestar-PS4/issues).

## 3.0.0

**USB-focused Lodestar.** Google Drive backup and NAS/SMB/NFS support are removed completely: their panel UI, API routes, OAuth/client state, FUSE client, SMB/NFS libraries, transfer queue, documentation, screenshots and tests are gone. Lodestar now concentrates on USB/external drives and local storage. The web library becomes a dense artwork grid: game details and actions open on demand instead of a permanent large inspector consuming half the screen.

**Español**

**Lodestar centrado en USB.** Se eliminan por completo las copias en Google Drive y la compatibilidad NAS/SMB/NFS: desaparecen su interfaz, rutas de API, estado de OAuth/cliente, cliente FUSE, bibliotecas SMB/NFS, cola de transferencias, documentación, capturas y pruebas. Lodestar se centra ahora en discos USB/externos y almacenamiento local. La biblioteca web pasa a ser una parrilla densa de carátulas: la ficha y las acciones de cada juego se abren bajo demanda en vez de ocupar media pantalla permanentemente.

## 2.0.2

**Public identity polish.** The README's main panel captures now show the Lodestar interface instead of the pre-rebrand panel; stale console, extension and upload captures are no longer presented as current UI. Google Cloud setup captures are explicitly marked as legacy examples because existing PS4Mount OAuth clients remain valid. User-facing errors, the log explainer, the extension identity and outbound HTTP requests now say Lodestar. Runtime compatibility names such as `/data/ps4mount`, USB `PS4Mount` folders and accepted legacy headers are unchanged.

**Español**

**Pulido de la identidad pública.** Las capturas principales del panel en el README muestran ahora la interfaz de Lodestar en vez del panel anterior al cambio de nombre; las capturas viejas de consola, extensión y subidas ya no se presentan como interfaz actual. Las capturas de Google Cloud quedan marcadas explícitamente como ejemplos legacy porque los clientes OAuth de PS4Mount ya creados siguen sirviendo. Los errores visibles, el explicador del log, la identidad de la extensión y las peticiones HTTP salientes ya dicen Lodestar. Los nombres operativos de compatibilidad como `/data/ps4mount`, las carpetas `PS4Mount` de los USB y las cabeceras legacy aceptadas no cambian.

## 2.0.1

**Agent protocol consistency and AutoRun safety.** The embedded panel, browser extension and download relay now use the canonical `X-Lodestar` / `X-Lodestar-Password` headers; the legacy `X-PS4Mount` names remain accepted for older clients. `GET /api/agent` and `GET /api/status` are documented in the user and agent workflows. AutoRun no longer selects a legacy `ps4mount-ps4` file as its launch target: legacy entries are removed or replaced, but only a `lodestar-ps4` artifact can be enabled, so an old installation cannot silently downgrade the boot payload.

**Español**

**Consistencia del protocolo de agentes y seguridad de AutoRun.** El panel integrado, la extensión y el relay de descargas usan ahora las cabeceras canónicas `X-Lodestar` / `X-Lodestar-Password`; los nombres legacy `X-PS4Mount` se siguen aceptando para clientes antiguos. `GET /api/agent` y `GET /api/status` quedan documentados en las guías de usuario y de agentes. AutoRun ya no elige un archivo legacy `ps4mount-ps4` como objetivo: esas entradas se quitan o se sustituyen, pero solo se puede activar un artefacto `lodestar-ps4`, evitando que una instalación antigua degrade silenciosamente el payload que arranca la consola.

## 2.0.0

**Lodestar: the project becomes independent.** PS4Mount PS4 is renamed **Lodestar** and is published as [felipebasurto/Lodestar-PS4](https://github.com/felipebasurto/Lodestar-PS4). It is not presented as a fork anymore: it started as a PS4 port of [PS4Mount](https://github.com/gasb07/ps4mount) by gasb07 (for the PS5) and keeps, as the GPL requires, the attribution to gasb07/PS4Mount as the origin project, its copyright notices and the GPL-3.0-or-later license.

- **Artifacts renamed**: the payload is `lodestar-ps4.elf` / `lodestar-ps4.bin`, the panel app `lodestar-panel.pkg` (home screen title "Lodestar") and the browser extension `lodestar-extension.zip`.
- **Operational names stay for compatibility**: `/data/ps4mount/` (settings, log, state), the `PS4Mount` scan folder on USB drives, `/data/PS4Mount` on the console's storage and the panel protocol headers (`X-PS4Mount`, `X-PS4Mount-Password`) keep their old names, so existing consoles, settings, libraries and extensions keep working. Nothing on a console that already uses it needs to be moved or renamed.
- **The panel's HTTP API is documented for agents and scripts** (README, and the full route table in AI-WORKFLOW.md section 3): the library, settings, log, scans, uploads, downloads from links and Google Drive can be driven by an agent with a terminal on the local network.
- **Panel app**: visible texts are now Lodestar's; it still shows the same panel (port 8770 unchanged) and closes with Back on the first page.

**Español**

- **Lodestar: el proyecto se independiza.** PS4Mount PS4 pasa a llamarse **Lodestar** y se publica como [felipebasurto/Lodestar-PS4](https://github.com/felipebasurto/Lodestar-PS4). Ya no se presenta como un fork: nació como un port para PS4 de [PS4Mount](https://github.com/gasb07/ps4mount) de gasb07 (para PS5) y conserva, como exige la GPL, la atribución a gasb07/PS4Mount como proyecto origen, sus avisos de copyright y la licencia GPL-3.0-or-later.
- **Artefactos renombrados**: el payload es `lodestar-ps4.elf` / `lodestar-ps4.bin`, la app del panel `lodestar-panel.pkg` y la extensión del navegador `lodestar-extension.zip`.
- **Los nombres operativos no cambian, por compatibilidad**: `/data/ps4mount/`, la carpeta `PS4Mount` de los USB, `/data/PS4Mount` en la memoria interna y las cabeceras del protocolo del panel (`X-PS4Mount`) siguen como estaban: las consolas, los ajustes, las bibliotecas y las extensiones que ya lo usan siguen funcionando sin mover ni renombrar nada.
- **La API HTTP del panel queda documentada para agentes y scripts** (README, y la tabla completa de rutas en AI-WORKFLOW.md, sección 3).

## 0.7.4-fix

Fixes on top of 0.7.4-beta.

- **Pkgs organized by default** (`organize` on): the .pkg files are moved into `ps4/<title ID>/` on the drive, so the console's own installer does not offer them again as installable games.
- **An add-on the installer refuses is not tried again on every load**: it is kept in `/data/ps4mount/dlcfail.txt` (content id, size and digest) and only tried again when that pkg changes. Delete the file to try them all again.
- **Games from the console's storage**: the pkg in the hold folder is linked with its own path (never through the `/data` alias, which the system cannot mount) and, when the path is longer than the 95 characters the system keeps, with a short link in `/data/ps4mount/l/`. Long names no longer leave a game added but unable to start.
- **Add-ons already installed are not installed again**: an add-on folder left mounted by an older version no longer hides the ones the system installed, which were copied again on every registration.

**Español**

- **Los pkgs se organizan por defecto** (`organize` activado): los .pkg se mueven a `ps4/<ID del título>/` en la unidad, así el instalador de la consola no los vuelve a ofrecer como juegos instalables.
- **Un DLC que el instalador rechaza no se reintenta en cada carga**: queda en `/data/ps4mount/dlcfail.txt` (content id, tamaño y digest) y solo se prueba otra vez cuando cambia ese pkg. Borrar el archivo fuerza a intentarlos todos de nuevo.
- **Juegos de la memoria interna**: el pkg de la carpeta hold se enlaza con su ruta real (nunca por el alias `/data`, que el sistema no puede montar) y, si la ruta pasa de los 95 caracteres que guarda el sistema, con un enlace corto en `/data/ps4mount/l/`. Los nombres largos ya no dejan un juego añadido pero sin poder arrancar.
- **Los DLC ya instalados no se vuelven a instalar**: una carpeta de DLC que dejó montada una versión anterior ya no tapa los que instaló el sistema, que se copiaban otra vez en cada registro.

## 0.7.4-beta

The network share works on **firmware 6.72** too (and, experimental, on every firmware itemzflow has a FUSE patch for, 5.05 to 13.50), and adding a NAS library no longer reads it once and again: each game is read once and kept, while the console is on and between restarts, and only new or changed pkgs are opened. The panel stays usable while a drive is walked or a game is read, long pkg paths no longer leave a game half registered, and a game whose update the system refuses (Bloodborne) is added without it. Also from the 6.72 tester's report: the notice shows the panel's real address and says when the games are ready, a first start waits for the settings, the share can be checked less often or only by hand, and several games can be removed at once.

**Upgrading with games on a NAS**: their paths change once (see below), so they are added again and the console asks for one restart.

**Español**

- **NAS en el firmware 6.72**, además del 13.52: el parche que enciende FUSE va por firmware, y el de 6.72 (el de itemzflow) ya está probado en consola.
- **NAS experimental de 5.05 a 13.50**: los parches de FUSE de itemzflow para los otros 25 firmwares, convertidos tal cual (los de 6.72 y 13.52 salen idénticos a los probados). El panel avisa de que en ese firmware es experimental.
- **Añadir juegos ya no relee la biblioteca entera**: cada juego se lee una vez y se guarda por unidad (`/data/ps4mount/titles-<unidad>.txt`), y solo se abren los pkgs nuevos o cambiados y los del grupo que se está registrando. Al añadir, un pkg por juego en vez de la biblioteca otra vez.
- El **panel** pide la lista de una en una, no pide nada con la pestaña en segundo plano, y sigue contestando mientras se recorre una unidad o se lee un juego, también **Parar**.
- **Rutas largas**: el sistema corta la ruta de un pkg a 95 caracteres y el juego quedaba a medias. PS4Mount le da un enlace corto (`/data/ps4mount/l/<TITLE_ID>-app.pkg`, `-patch.pkg`, `-ac-<DLC>.pkg`, uno por DLC) cuando la ruta no cabe; sirve igual para USB y NAS.
- **El NAS tiene siempre la misma ruta** (`/data/ps4mount/nas`, un enlace al montaje): volver a cargar PS4Mount en el mismo arranque lo montaba en otra carpeta (`nas-2`, `nas-3`…), y eso releía la biblioteca entera y volvía a añadir todos sus juegos pidiendo reiniciar. Ahora no se lee nada ni se añade nada.
- **App "PS4Mount" en la pantalla de inicio**: muestra el panel a pantalla completa, más grande para la tele, dentro de la propia app, así que el navegador de la consola no se toca (no estorba a los desbloqueos por WebKit). Círculo cierra la ventana abierta y, en la página principal, la app. PS4Mount la descarga de esta versión (`ps4mount-panel.pkg`) y la instala sola si la consola no la tiene, y la primera vez la abre; si la borras, no vuelve a ponerla. También se puede instalar a mano.
- **Un solo montaje del NAS por arranque**: la consola tiene 8 dispositivos FUSE y cada carga de PS4Mount ocupaba uno (no se puede desmontar), así que tras unas cuantas recargas el NAS dejaba de montarse. Ahora una carga nueva usa el montaje de la anterior; solo una versión con otro código del NAS monta uno nuevo. Si aun así se llenan, el aviso dice que hay que reiniciar.
- **Ajustes → NAS → Buscar juegos nuevos en él**: cada minuto (como antes), cada 10 minutos, cada hora o **solo con Buscar juegos**.
- El aviso de arranque da la **dirección real del panel** (`http://192.168.1.50:8770`), y al terminar el primer análisis avisa de cuántos juegos están **listos para jugar** (abrir uno antes daba "datos dañados").
- **Primera vez**: si en la consola no hay nada de PS4Mount, no se añade ningún juego hasta guardar los Ajustes una vez (antes, un USB ya conectado se añadía entero con los valores por defecto). Quien ya lo usaba no nota nada.
- **Quitar varios**: botón en el panel para quitar de la biblioteca los juegos elegidos (o todos) de una vez.
- **Enviar paquetes** explica cómo activar la memoria de la consola como destino cuando está apagada.
- El panel ya no deja juegos "en marcha" que el sistema no llegó a abrir o que se cerraron con error; el log dice cuándo el sistema cierra un juego que no se suspendió a tiempo (lo que pierde el progreso sin guardar).
- Una conexión `https://` al panel se cierra al momento (el panel es solo `http://`), y el README lo explica.
- El **log** dice qué está haciendo el análisis (`checking N pkg(s), M new or changed`, `M read in X s`) y solo cuenta las lecturas de verdad: una línea por MB ya no llena el log.
- **Actualizaciones que el sistema rechaza** (Bloodborne 01.09): el juego se añade **sin la actualización**, y esa actualización no se vuelve a usar; si el pkg cambia (otra actualización) se prueba de nuevo, y también con **Buscar juegos**. Las marcas que dejaron versiones anteriores se siguen respetando.

**English**

- **NAS on firmware 6.72**, as well as 13.52: the patch that enables FUSE is per firmware, and the 6.72 one (itemzflow's) is tested on a console.
- **Experimental NAS on 5.05 to 13.50**: itemzflow's FUSE patches for the other 25 firmwares, converted one for one (those of 6.72 and 13.52 come out the same as the tested ones). The panel says it is experimental on that firmware.
- **Adding games no longer reads the whole library again**: each game is read once and kept per drive (`/data/ps4mount/titles-<drive>.txt`), and only new or changed pkgs and those of the group being registered are opened. Adding a game costs one pkg; the library is not read again.
- The **panel** asks for the list one request at a time, asks for nothing in a background tab, and answers while a drive is walked or a game is read, **Stop** included.
- **Long paths**: the system cuts a pkg path at 95 characters, which left a game half registered. PS4Mount hands it a short link (`/data/ps4mount/l/<TITLE_ID>-app.pkg`, `-patch.pkg`, `-ac-<ADD-ON>.pkg`, one per add-on) when the path does not fit; it works for USB drives and NAS alike.
- **The NAS always has the same path** (`/data/ps4mount/nas`, a link to the mount): loading PS4Mount again in the same boot mounted it in another folder (`nas-2`, `nas-3`…), which read the whole library again and added all its games again, asking for a restart. Now nothing is read and nothing is added.
- **"PS4Mount" app on the home screen**: it shows the panel full screen, larger for a TV, inside the app itself, so the console's web browser is left alone (WebKit jailbreaks are not disturbed). Circle closes the open window and, on the first page, the app. PS4Mount downloads it from this release (`ps4mount-panel.pkg`) and installs it when the console does not have it, and a first start opens it; deleted, it is not put back. It can be installed by hand too.
- **One NAS mount per boot**: the console has 8 FUSE devices and each load of PS4Mount took one (it cannot be unmounted), so after a few reloads the share would not mount any more. A new load now uses the mount of the one before; only a version with other NAS code mounts a new one. If they fill up anyway, the notice says to restart.
- **Settings → NAS → Look for new games on it**: every minute (as before), every 10 minutes, every hour or **only with Scan for games**.
- The start notice gives the panel's **real address** (`http://192.168.1.50:8770`), and when the first scan ends it says how many games are **ready to play** (starting one earlier said its data was corrupted).
- **First start**: when the console has nothing of PS4Mount yet, games are added only once the Settings are saved, or with the defaults after 2 minutes with nobody at the panel (a USB drive plugged in already was added whole at once). Existing users notice nothing.
- **Remove several**: a panel button removes the chosen games (or all) from the library at once.
- **Send packages** says how to turn on the console's storage as a destination when it is off.
- The panel no longer shows games as running when the system never started them or they closed with an error; the log says when the system closes a game that did not suspend in time (which loses unsaved progress).
- An `https://` connection to the panel is closed at once (the panel is `http://` only), and the README says so.
- The **log** says what the scan is doing (`checking N pkg(s), M new or changed`, `M read in X s`) and only counts real reads: a line per MB no longer fills it.
- **Updates the system refuses** (Bloodborne 01.09): the game is added **without the update**, and that update is not used again; when the pkg changes (another update) it is tried again, and with **Scan for games** too. The marks left by earlier versions are still honoured.

## 0.6.2-beta

Homebrew **ports** (games with a title ID of their own instead of CUSA, such as SMW or Smash Ultimate) are played from the pkg like any game; **big add-ons** are installed on their own, so games see them; and small **homebrew apps** can be sent from the panel and installed. Tested on a PS4 on firmware 13.52 with GoldHEN: SMW (`SMWR00001`, 5.5 GB) from a NAS over SMB without installing it; Marvel's Spider-Man's three story add-ons (7.3 GB) installed from the NAS in about a minute and a half, all of them showing in the game; PS4SX2 (a 50 MB emulator) sent from the panel and installed.

**Español**

- **Ports homebrew**: los juegos que no son CUSA (`SMWR00001`, `SSBU00001`…) se usan desde el pkg como cualquier juego, desde un USB, la memoria interna o un NAS. Como un port y una app homebrew son iguales por dentro, se distinguen por el peso: lo que no es CUSA se usa si pesa **1 GB o más** (un port lleva los datos del juego); las apps, de pocos MB, se siguen instalando de la forma normal.
- Un port o una app que ya tengas **instalado de la forma normal no se toca**: para jugarlo desde el pkg, desinstálalo primero (las partidas guardadas se conservan).
- Las apps del sistema (`NPXS`) y los juegos de PS5 (`PPSA`) no se usan nunca.
- Al actualizar, los pkgs que la versión anterior había descartado se vuelven a revisar con las reglas nuevas.
- **DLC grandes (más de 256 MB)**: se **instalan solos** con el instalador del sistema, **en segundo plano**, porque un juego solo ve un DLC que el sistema tiene anotado como instalado (antes se montaban y el juego no los veía). Ocupan memoria interna: antes se comprueba que haya sitio, y se avisa al empezar y cuando cada uno está listo. Se puede apagar en **Ajustes → Instalar también los DLC grandes**.
- **Enviar e instalar apps homebrew**: en **Enviar paquetes**, una app de menos de 1 GB ya no se rechaza sin más: sale como **Lista para instalar**, se envía a la consola y se instala con el instalador del sistema; la copia se borra cuando ya está instalada.

**English**

- **Homebrew ports**: games that are not CUSA (`SMWR00001`, `SSBU00001`…) are used from the pkg like any game, from a USB drive, the console's storage or a NAS. A port and a homebrew app look the same inside, so they are told apart by size: what is not CUSA is used when it weighs **1 GB or more** (a port carries the game's data); apps, a few MB, are still installed the usual way.
- A port or app already **installed the usual way is left alone**: to play it from the pkg, uninstall it first (saved games are kept).
- System apps (`NPXS`) and PS5 games (`PPSA`) are never used.
- On upgrade, the pkgs the previous version had turned down are checked again with the new rules.
- **Big add-ons (over 256 MB)** are **installed on their own** with the system's installer, **in the background**, because a game only sees an add-on the system has recorded as installed (they used to be mounted, and games did not see them). They take internal space: there has to be room first, and there is a notice when they start and when each one is ready. It can be turned off in **Settings → Also install big add-ons**.
- **Send and install homebrew apps**: in **Send packages**, an app under 1 GB is no longer just turned down: it shows as **Ready to install**, is sent to the console and installed with the system's installer; the copy is deleted once it is installed.

## 0.6.1-beta

Two big changes. Games are now used **where they are** by default: PS4Mount no longer moves or renames their pkgs unless you turn on `Organize pkgs into the ps4 folder`. And games can be played **from a network share (NAS)**, over SMB or NFS, without copying or installing them, on firmware 13.52: the panel finds the computers and NAS boxes on the network and lists their shared folders, so nothing has to be typed. The catalog code comes from [EzioRamesha's fork](https://github.com/EzioRamesha/ps4mount), adapted to keep the old `ps4/<TITLE_ID>` layout working. Tested on a PS4 on firmware 13.52 with GoldHEN: 31 pkgs on a USB drive (base games, updates, add-ons, merged pkgs and a duplicate copy); Marvel's Spider-Man (42 GB, with its 1.19 update and 4 add-ons) played from a Linux PC over SMB, and other games over NFS; the share found and listed on its own; console on Wi-Fi and on a cable straight to the PC.

**Español**

- **Nuevo ajuste "Organizar los pkgs en la carpeta ps4"**, **apagado por defecto**: los `.pkg` se usan **donde están, en cualquier carpeta de la unidad y con su nombre** (en la memoria interna se buscan solo `/data/PS4Mount` y `ps4`), y PS4Mount los agrupa por el título que llevan dentro (juego, actualización y DLC), sin renombrar ni mover nada. Al activarlo se mueven a `ps4/<TITLE_ID>/`, como antes.
- Al **apagar Organizar**, los juegos que PS4Mount ya había organizado **vuelven solos a su sitio y con su nombre originales** (lo recuerda en `origin.txt` de cada juego) y se usan desde ahí; los que organizaste tú a mano se quedan donde están. La primera vez hay que reiniciar la consola una vez, como al añadir un juego.
- Los juegos **ya organizados** en `ps4/<TITLE_ID>/` sin origen conocido se usan **en su sitio**, sin moverlos: la biblioteca antigua se sigue usando igual, y las configuraciones sin el ajuste pasan a este modo al actualizar.
- Los pkgs **base+actualización fusionados** funcionan: una actualización suelta más vieja que el juego no se usa, así que no lo hace retroceder de versión.
- Cada unidad se agrupa por separado: una actualización o un DLC que esté en **otra** unidad no se usa, para que desenchufar una no rompa los juegos de las demás.
- Borrar desde la consola un juego usado donde está **no borra tus pkgs**: lo quita de la biblioteca y lo marca como quitado (los pkgs son tuyos). **Restaurar los archivos originales** no aplica en ese modo: no hay nada que devolver.
- La búsqueda completa de la unidad (para encontrar pkgs nuevos) se hace cada minuto o al pulsar **Buscar juegos**; entre medias se comprueban los juegos ya conocidos. El catálogo se guarda en `/data/ps4mount/catalog-<unidad>.txt`, así que un reinicio no vuelve a leer todos los pkgs.
- **Juegos desde una NAS (SMB o NFS)**, en **Ajustes → NAS**: los juegos de una carpeta compartida en un PC con Windows, Linux o macOS, o en una caja NAS (Synology, QNAP, un router con USB), se juegan **desde ahí, sin copiarlos ni instalarlos**, con su actualización y sus DLC. Es **solo lectura**: PS4Mount nunca escribe, renombra ni mueve nada en el recurso. Solo firmware **13.52**: en otros el ajuste sale desactivado.
- **Buscar servidores en la red**: la consola prueba toda su red en unos segundos y lista los equipos que comparten archivos (con su nombre, si lo dan). Al elegir uno, lista sus **carpetas compartidas**; si pide cuenta, lo dice y basta con poner usuario y contraseña. Debajo hay una ayuda para compartir una carpeta en Windows, macOS, Linux y una caja NAS.
- **Velocidad**: PS4Mount pide los datos por adelantado y varios a la vez, así que la red se aprovecha casi entera. Por Wi-Fi va lento; **con cable** se juega como desde el disco interno. Si no tienes switch ni un puerto libre en el router, sirve un cable directo del PC a la consola (compartiendo la conexión del PC por ese cable).
- Un servidor que no contesta (apagado, dirección mal escrita, cortafuegos) se avisa en **3 segundos**, sin dejar el panel bloqueado.
- **Si se corta la red** (un cable desenchufado, el servidor reiniciado), el juego **se queda en pausa** y sigue solo cuando vuelve, sin errores: PS4Mount reconecta y reabre los ficheros (espera hasta 2 minutos). Probado con 33 segundos sin cable en plena partida.
- El recurso **nunca se organiza** (con Organizar activado no se usa), y cambiar de protocolo, servidor o carpeta se hace al momento, sin reiniciar. Borrar desde la consola un juego del NAS **no borra sus pkgs**.
- Si PS4Mount arranca solo con la consola (AutoRun de GoldHEN), ese proceso no puede abrir los dispositivos FUSE (una limitación de GoldHEN): se vuelve a cargar **solo, en unos segundos y sin avisos de más**, por el BinLoader de GoldHEN (puerto 9090), que tiene que estar encendido.
- Volver a cargar PS4Mount en el mismo arranque monta el recurso en otra carpeta (`nas-2`, `nas-3`…): los juegos del NAS piden reiniciar una vez. Tras unas pocas cargas en el mismo arranque se acaban los dispositivos FUSE y hay que reiniciar.

**Otros cambios**

- **Menos avisos al arrancar**: uno solo, "en marcha". "Buscando juegos…" y los totales salen solo al pulsar **Buscar juegos**; al arrancar solo se avisa de lo nuevo.
- Los avisos de la copia que arranca con la consola salen **en el idioma de la consola** (antes, siempre en inglés): el idioma se lee del registro del sistema.
- **Sin internet nada se queda esperando**: un nombre que no se puede resolver (Drive, enlaces) falla en unos segundos en vez de casi un minuto, y cerrar PS4Mount ya no espera a eso.

**English**

- **New "Organize pkgs into the ps4 folder" setting**, **off by default**: the `.pkg` files are used **where they are, in any folder of the drive and with their own names** (on the console's storage, only `/data/PS4Mount` and `ps4` are searched), and PS4Mount groups them by the title they carry inside (game, update and add-ons), without renaming or moving anything. Turned on, they are moved to `ps4/<TITLE_ID>/` as before.
- **Turning Organize off** sends the games PS4Mount had organized **back to where they were, with their original names** (it remembers them in each game's `origin.txt`), and they are used from there; games you organized by hand stay where they are. The first time, restart the console once, as when adding a game.
- Games **already organized** in `ps4/<TITLE_ID>/` with no known origin are used **in place**, without moving them: the old library layout keeps working as usual, and configs without the setting switch to this mode on upgrade.
- **Merged base+update pkgs** work: a separate update older than the game is not used, so it does not take the game back to an older version.
- Each drive is grouped on its own: an update or add-on on **another** drive is not used, so unplugging one drive never breaks the games of the others.
- Deleting an in-place game from the console **does not delete your pkgs**: the game leaves the library and is marked as removed (the pkgs are yours). **Restore original files** does not apply in that mode: there is nothing to move back.
- The full walk of a drive (to find new pkgs) runs every minute or when **Scan for games** is pressed; in between, the games already known are checked. The catalog is saved in `/data/ps4mount/catalog-<drive>.txt`, so a restart does not read every pkg again.
- **Games from a NAS (SMB or NFS)**, in **Settings → NAS**: the games in a shared folder on a Windows, Linux or macOS computer, or on a NAS box (Synology, QNAP, a router with USB), are played **from there, without copying or installing them**, with their update and add-ons. It is **read only**: PS4Mount never writes to, renames or moves anything in the share. Firmware **13.52** only: on others the setting is disabled.
- **Find servers on the network**: the console tries its whole network in a few seconds and lists the machines that share files (with their name, when they give it). Picking one lists its **shared folders**; when it asks for an account the panel says so, and a user and password is all it takes. Below there is a short guide to sharing a folder on Windows, macOS, Linux and a NAS box.
- **Speed**: PS4Mount asks for the data ahead of time, several pieces at once, so the network is used almost in full. Over Wi-Fi it is slow; **over a cable** games play as from the internal drive. With no switch or free router port, a cable straight from the computer to the console works too (sharing the computer's connection on it).
- A server that does not answer (off, a mistyped address, a firewall) is reported in **3 seconds**, without freezing the panel.
- **If the network drops** (a cable pulled, the server restarted), the game **pauses** and goes on by itself when it is back, with no errors: PS4Mount reconnects and reopens the files (it waits up to 2 minutes). Tested with 33 seconds without a cable in the middle of a game.
- The share is **never organized** (it is not used with Organize on), and changing the protocol, server or folder happens at once, with no restart. Deleting a NAS game on the console **does not delete its pkgs**.
- If PS4Mount starts with the console (GoldHEN's AutoRun), that process cannot open the FUSE devices (a GoldHEN limitation): it loads itself again **on its own, in a few seconds and without extra notices**, through GoldHEN's BinLoader (port 9090), which has to be on.
- Loading PS4Mount again in the same boot mounts the share in another folder (`nas-2`, `nas-3`…): the NAS games ask for one restart. After a few loads in the same boot the FUSE devices run out and the console has to be restarted.

**Other changes**

- **Fewer notices at start**: just one, "running". "Scanning…" and the totals only show when **Scan for games** is pressed; at start only what is new is reported.
- The notices of the copy that starts with the console are **in the console's language** (they were always in English): the language is read from the system registry.
- **With no internet nothing hangs**: a name that cannot be resolved (Drive, links) fails in a few seconds instead of almost a minute, and stopping PS4Mount no longer waits for it.

## 0.4.17-beta

The `.bin` works with older GoldHEN versions too (such as 2.4b18): from Payload Guest, Vue or the BinLoader on port 9090. Tested on firmware 13.52 with GoldHEN (port 9090 and Payload Guest): not yet with an older GoldHEN.

**Español**

- El `.bin` ahora es compatible con versiones viejas de GoldHEN (como la 2.4b18): desde Payload Guest, Vue o el BinLoader del puerto 9090.

**English**

- The `.bin` now works with older GoldHEN versions (such as 2.4b18): from Payload Guest, Vue or the BinLoader on port 9090.

## 0.4.16-beta

Games whose pkg asks for a newer firmware than the console's, made to run with a backport, are no longer refused. Tested on firmware 13.52 only: not yet on an older console.

**Español**

- Un pkg hecho para un firmware más nuevo que el de la consola ya no se rechaza (con un backport funciona): se añade, se envía o se descarga igual, con un aviso.

**English**

- A pkg built for a newer firmware than the console's is no longer refused (a backport makes it run): it is added, sent or downloaded as usual, with a warning.

## 0.4.15-beta

Downloads from links, and a browser extension that sends downloads to the console. Tested on a PS4 on firmware 13.52 with GoldHEN: a pkg and a RAR in 5 parts from a computer on the network, pkgs over HTTPS from file hosts, a 24.6 GB game in a RAR5 with its file list encrypted, in 3 parts with a code before each name, its password tried by itself (the game plays), a ZIP with AES; the extension in Brave. (The version number jumps from 0.2.0: no 0.3.)

**Español**

- **Importar archivos desde URL**, con soporte para PKG, ZIP y RAR. Las partes de un RAR se agrupan y se avisa si falta alguna.
- Un RAR o ZIP se descomprime mientras baja y solo se sacan sus pkgs; con poco espacio, cada parte se borra al descomprimirse.
- Cada pkg se comprueba con sus primeros 4 MB, como al enviar desde el PC: uno que no sirve para la descarga ahí.
- Sigue sola tras un corte de red o al volver a cargar PS4Mount; la consola no entra en reposo mientras descarga. Sigue las redirecciones, pide de nuevo un enlace que caduca y baja el número de conexiones si el servidor lo pide.
- **RAR5 con contraseña**, también con la lista de archivos cifrada (`rar -hp`) y en partes: se escribe al añadir la descarga, o al pulsar Reintentar si falta. Se prueban solas las que ya abrieron otra descarga (`/data/ps4mount/passwords.txt`) y el sitio entre corchetes al principio del nombre (`[SITIO]-juego`); una equivocada se nota al instante, sin bajar el resto. También ZIP con contraseña, clásica o AES.
- Junta las partes de un RAR aunque el sitio ponga un código distinto delante de cada nombre (`abc-juego.part1.rar`, `xyz-juego.part2.rar`).
- No abre RAR4 con contraseña, 7z, ni enlaces que piden iniciar sesión.
- **Extensión del navegador** (Chrome, Brave, Edge, Firefox; se carga a mano desde `ps4mount-extension.zip`): las descargas de pkg, ZIP y RAR van a la consola en vez de al ordenador; las partes de un RAR esperan en una cesta hasta pulsar Enviar, con la contraseña si hace falta.
- Experimental, en la extensión: el navegador baja también parte de los bloques y se los pasa a la consola por la red local (puerto 8771); si se cierra, la consola sigue sola.

**English**

- **Import files from URLs**, with support for PKG, ZIP and RAR. Multipart RAR archives are grouped and missing parts are reported.
- A RAR or ZIP is extracted while it downloads and only its pkgs come out; with little space, each part is deleted once extracted.
- Every pkg is checked from its first 4 MB, as when sending from a computer: one that will not do stops the download there.
- It goes on by itself after a lost connection or when PS4Mount loads again; the console stays out of rest mode while it downloads. It follows redirects, asks again for a link that expires and uses fewer connections when the server asks for it.
- **RAR5 with a password**, also with its file list encrypted (`rar -hp`) and in parts: written when adding the download, or when pressing Try again if it is missing. The ones that opened another download (`/data/ps4mount/passwords.txt`) and the site in brackets at the start of the name (`[SITE]-archive`) are tried by themselves; a wrong one shows at once, without downloading the rest. ZIPs with a password too, classic or AES.
- The parts of a RAR join up even when the host puts a different code before each name (`abc-game.part1.rar`, `xyz-game.part2.rar`).
- It does not open older RARs (4) with a password, 7z, nor links that need a sign-in.
- **Browser extension** (Chrome, Brave, Edge, Firefox; loaded by hand from `ps4mount-extension.zip`): pkg, ZIP and RAR downloads go to the console instead of the computer; the parts of a RAR wait in a basket until Send, with the password if needed.
- Experimental, in the extension: the browser fetches part of the blocks too and hands them to the console over your network (port 8771); closed, the console goes on by itself.

## 0.2.0-beta

Google Drive backups. On firmware 13.52 with GoldHEN, an upload and restore were tested with a 7.2 GB package and with a group of related packages.

**Español**

- **Copias en Google Drive**, opcionales en esta versión histórica. La consola permitía guardar y restaurar copias de paquetes de la biblioteca.
- Cada usuario usa **sus propias credenciales de Google** (un proyecto gratuito en modo prueba: la guía está en el README), que se quedan solo en su consola. El inicio de sesión es con un código en google.com/device desde el móvil. PS4Mount solo ve los archivos que él crea en tu Drive (permiso `drive.file`).
- Subida a la velocidad de tu conexión; bajada a unos 15 MB/s, el máximo de la PS4, con 12 conexiones a la vez. Cada archivo se comprueba con su MD5.
- Una transferencia cortada (red, detener PS4Mount, apagar) sigue sola desde donde iba la próxima vez que cargas PS4Mount. Mientras transfiere, la consola no entra en reposo.
- Progreso en el panel (porcentaje, velocidad, tiempo que queda) y avisos en la pantalla de la PS4 al empezar, cada 25 % y al terminar. Si Google pide conectar de nuevo, el código sale en la pantalla.
- Política de privacidad: [PRIVACY.md](PRIVACY.md).
- Arreglos: un DLC que el instalador rechaza ya no impide añadir su juego (se avisa una vez y se reintenta la próxima vez); los DLC se instalan después de registrar el juego; un juego bajado de Drive se añade una sola vez, con todos sus pkgs; el aviso de reiniciar se mantiene al recargar PS4Mount; todos los avisos salen en el idioma de la consola.

**English**

- **Google Drive backups**, optional: the console uploads your games (with their update and add-ons) to your Drive and downloads them back to a USB drive or the console's storage, no computer needed. **Back up to Drive** uploads the whole library and skips what is already there; **Send to Drive** on a game's page; the **Google Drive** window shows your account, your space and your games in Drive, with **Download** and **Delete from Drive** (to the trash).
- Each user uses **their own Google credentials** (a free project in testing: the guide is in the README), which stay on their console only. Signing in is with a code at google.com/device from a phone. PS4Mount only sees the files it creates in your Drive (`drive.file` permission).
- Uploads at your line's speed; downloads at about 15 MB/s, the PS4's most, over 12 connections at once. Every file is checked with its MD5.
- A cut transfer (network, stopping PS4Mount, a power off) goes on by itself from where it was the next time PS4Mount loads. While it transfers, the console does not go to rest mode.
- Progress in the panel (percentage, speed, time left) and notices on the PS4's screen when it starts, every 25% and when it ends. If Google asks to connect again, the code shows on the screen.
- Privacy policy: [PRIVACY.md](PRIVACY.md).
- Fixes: an add-on the installer refuses no longer keeps its game out (one notice, retried the next time); add-ons are installed after their game is registered; a title restored from a Drive backup is added once, with all its packages; the restart notice survives a reload of PS4Mount; every notice is in the console's language.

Load `ps4mount-ps4.bin` with Payload Guest (copy it to `/data/payloads/`), or send `ps4mount-ps4.elf` to GoldHEN's payloader (port 9090).

## 0.1.0-beta

First version of the PS4 port. Tested on a PS4 on firmware 13.52 with GoldHEN.

**Español**

- Monta paquetes de PS4 compatibles desde un USB exFAT (también pkgs de más de 4 GB) o desde la memoria de la consola. Solo usa lo que pongas en su carpeta `PS4Mount` de cada disco (y `/data/PS4Mount`): el resto, incluido `/data/pkg`, no lo toca.
- Actualizaciones, y DLC de hasta 256 MB instalados con el instalador del sistema en segundos (aparecen en la información del juego).
- Panel web (`http://<IP de la PS4>:8770`) con la biblioteca, portadas y una ficha por juego. Hay una versión sencilla para navegadores antiguos, sin probar todavía en el de la PS4.
- Enviar paquetes desde el PC por la red, comprobando cada pkg antes de enviarlo (compatibilidad, firmware, integridad y espacio libre), con cancelar y enviar de nuevo.
- Borrar un juego desde la consola borra también sus pkgs; *Quitar de la biblioteca* los conserva. Mientras la consola borra, PS4Mount aparta las carpetas de los juegos: el borrado del sistema nunca llega a los pkgs.
- Carpetas por juego al copiar (hasta dos niveles: `Juego/Game`, `Juego/Update`, `Juego/DLC`).
- Los juegos de un USB desconectado se muestran como *Unidad no conectada*, y abrir uno lo explica.
- Panel y notificaciones en español e inglés, según el idioma de la consola o los ajustes.
- Limitación: un juego nuevo (o una actualización que llega después) necesita reiniciar la consola una vez.

**English**

- Mounts compatible PS4 packages from an exFAT USB drive (pkgs over 4 GB too) or from the console's storage. Only what you put in its `PS4Mount` folder of each drive (and `/data/PS4Mount`) is used: nothing else, `/data/pkg` included, is touched.
- Updates, and add-ons up to 256 MB installed with the system's installer in seconds (they show in the game's information).
- Web panel (`http://<PS4 IP>:8770`) with the library, artwork and a page per game. There is a simpler page for older browsers, not yet tested on the PS4's own browser.
- Sending packages from a computer over the network, every pkg checked before it is sent (package compatibility, firmware, integrity and available space), with cancel and send again.
- Deleting a game on the console deletes its pkgs too; *Remove from library* keeps them. While the console deletes, PS4Mount takes the game folders away: the system's delete never reaches the pkgs.
- A folder per game when copying (two levels down: `Game/Game`, `Game/Update`, `Game/DLC`).
- Games of an unplugged USB drive show as *Drive not connected*, and starting one says why.
- Panel and notifications in English and Spanish, following the console's language or the settings.
- Limitation: a new game (or an update that arrives later) needs one console restart.

Load `ps4mount-ps4.bin` with Payload Guest (copy it to `/data/payloads/`), or send `ps4mount-ps4.elf` to GoldHEN's payloader (port 9090).
