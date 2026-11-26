/* Texts of both panel pages in other languages, by their English text (English is the
 * page itself, and the fallback for a text a table lacks). build.sh puts this file in
 * the pages where they say <!--LANG-->. A new language: a table here and its tag in
 * src/lang.c. ES5 only: the browser of older firmwares reads it too. */
var LANGS = {
  es: {
    /* fixed texts of the pages */
    'external game library control for PlayStation 4': 'control de la biblioteca externa de juegos para PlayStation 4',
    'Search game or CUSA': 'Buscar juego o CUSA',
    'Scan for games': 'Buscar juegos',
    'Send games': 'Enviar juegos',
    'Settings': 'Ajustes',
    'Log': 'Log',
    'Notifications': 'Notificaciones',
    'Stop': 'Detener',
    'Games': 'Juegos',
    'Library': 'Biblioteca',
    'Manage': 'Gestionar',
    'Search and filter': 'Buscar y filtrar',
    'No matching games': 'Ningún juego coincide',
    'Back': 'Volver',
    'Save': 'Guardar',
    'Cancel': 'Cancelar',
    'Close': 'Cerrar',
    'No PS4 games found. Put the .pkg files (game, update and add-ons) in the PS4Mount folder of a USB drive.':
      'No hay juegos de PS4. Pon los .pkg (juego, actualización y DLC) en la carpeta PS4Mount de un USB.',
    'No PS4 games found. Put the .pkg files (game, update and add-ons) on a USB drive, then Scan for games or send them from this page.':
      'No hay juegos de PS4. Pon los .pkg (juego, actualización y DLC) en un USB y pulsa Buscar juegos o envíalos desde esta página.',
    'Adding games': 'Añadir juegos',
    'Folders to search for pkgs (one per line, relative to each drive)':
      'Carpetas donde buscar pkgs (una por línea, dentro de cada unidad)',
    '/ is the root of the drive. The ps4 folder holds organized games, so it cannot be used here.':
      '/ es la raíz de la unidad. La carpeta ps4 guarda los juegos organizados, así que no se puede usar aquí.',
    '/ is the root of the drive. Games are moved into the ps4 folder, so it cannot be used here.':
      '/ es la raíz de la unidad. Los juegos se mueven a la carpeta ps4, así que no se puede usar aquí.',
    'Add new games in groups': 'Añadir los juegos nuevos por grupos',
    'Games per group': 'Juegos por grupo',
    'Smaller groups are gentler on the system when many games are added at once.':
      'Los grupos pequeños cargan menos el sistema cuando se añaden muchos juegos a la vez.',
    'Organize pkgs into the ps4 folder': 'Organizar los pkgs en la carpeta ps4',
    'On (the default): the .pkg files are moved into ps4/<title ID>/ on the drive, so the console\'s own installer does not find them again. Off: they are used where they are, anywhere on the drive, and are not renamed or moved.':
      'Activado (por defecto): los .pkg se mueven a ps4/<ID del título>/ en la unidad, así el instalador de la consola no los encuentra otra vez. Desactivado: se usan donde están, en cualquier parte de la unidad, sin renombrarlos ni moverlos.',
    'External drives and scan folders': 'Unidades externas y carpetas de búsqueda',
    'Add-ons (DLC)': 'DLC',
    'Mount from the source drive': 'Montar desde la unidad de origen',
    'Console storage (installs add-ons, up to 256 MB)': 'Memoria de la consola (instala los DLC, hasta 256 MB)',
    'Off': 'Apagado',
    'Also install big add-ons (over 256 MB)': 'Instalar también los DLC grandes (más de 256 MB)',
    "Also use pkgs in the console's /data/PS4Mount": 'Usar también los pkgs de /data/PS4Mount de la consola',
    "Pkgs sent to the console's storage are added from there, with no install to wait for. They still take internal space.":
      'Los pkgs enviados a la memoria de la consola se añaden desde ahí, sin esperar una instalación. Ocupan espacio interno.',
    "Console storage: turn on \"Also use pkgs in the console's /data/PS4Mount\" in Settings":
      'Memoria de la consola: activa "Usar también los pkgs de /data/PS4Mount de la consola" en Ajustes',
    'External mode mounts add-on packages from the source drive. Console storage installs add-ons up to 256 MB each, or any size with the big-add-ons option. Off disables add-ons managed by Lodestar.':
      'El modo externo monta los paquetes de DLC desde la unidad de origen. El modo interno instala DLC de hasta 256 MB cada uno, o de cualquier tamaño con la opción de DLC grandes. Apagado desactiva los DLC gestionados por Lodestar.',
    'System and GoldHEN': 'Sistema y GoldHEN',
    "Read the console's log (game start and stop)": 'Leer el log de la consola (inicio y fin de juego)',
    "On by default. Turn it off to leave the log to another payload: AnyPad needs it to tell its virtual pad apart, and only one payload can read it. Games in play are then noticed a few seconds later, from their mount.":
      'Activado por defecto. Desactivalo para dejarle el log a otro payload: AnyPad lo necesita para distinguir su pad virtual, y solo un payload puede leerlo. Los juegos en marcha se notan unos segundos más tarde, por su montaje.',
    'Remove several': 'Quitar varios',
    'Remove games from the library': 'Quitar juegos de la biblioteca',
    "The chosen games leave the console's home screen. Their pkgs stay where they are: nothing is deleted, and Add to library brings each one back.":
      'Los juegos elegidos salen de la pantalla de inicio de la consola. Sus pkgs se quedan donde están: no se borra nada, y Añadir a la biblioteca devuelve cada uno.',
    'Remove the chosen ones': 'Quitar los elegidos',
    'No games to remove (an open game is closed first)': 'No hay juegos que quitar (cierra antes el juego abierto)',
    'Remove {0} game(s) from the library? Their pkgs stay where they are.': '¿Quitar {0} juego(s) de la biblioteca? Sus pkgs se quedan donde están.',
    '{0} removed; not removed: {1}': '{0} quitado(s); sin quitar: {1}',
    '{0} game(s) removed from the library': '{0} juego(s) quitado(s) de la biblioteca',
    'Removing {0} of {1}…': 'Quitando {0} de {1}…',
    'First start: check the settings (which folders, games per group, add-ons) and press Save in Settings. While this page is open no game is added; closed for 2 minutes without saving, the default settings are used.':
      'Primera vez: revisa los ajustes (qué carpetas, juegos por grupo, DLC) y pulsa Guardar en Ajustes. Mientras esta página esté abierta no se añade ningún juego; si se cierra 2 minutos sin guardar, se usan los ajustes por defecto.',
    'Language': 'Idioma',
    'Restore original files': 'Restaurar archivos originales',
    'Takes games off the console and puts their pkgs (game, update and add-ons) back where they were, with the names they had. Nothing is deleted. The open game is closed first.':
      'Quita juegos de la consola y vuelve a dejar sus pkgs (juego, actualización y DLC) donde estaban, con el nombre que tenían. No se borra nada. Antes se cierra el juego abierto.',
    'Takes the game off the console and puts its pkgs back where they were, with the names they had. Nothing is deleted.':
      'Quita el juego de la consola y vuelve a dejar sus pkgs donde estaban, con el nombre que tenían. No borra nada.',
    'Restore the files of all games': 'Restaurar los archivos de todos los juegos',
    'Restore the files of chosen games': 'Restaurar los archivos de los juegos elegidos',
    'Use restored pkgs again': 'Volver a usar los pkgs restaurados',
    'Each chosen game gets its pkgs back where they were, with its update and all its add-ons. The open game is closed first.':
      'A cada juego elegido se le devuelven sus pkgs donde estaban, con su actualización y todos sus DLC. Antes se cierra el juego abierto.',
    'Files': 'Archivos',
    'Restore the chosen ones': 'Restaurar los elegidos',
    'Restore the original files of {0}?\n\nThe game leaves the console and its pkgs (game, update and add-ons) go back where they were, with the names they had. The open game is closed first. Lodestar stops using these pkgs. Nothing is deleted.':
      '¿Restaurar los archivos originales de {0}?\n\nEl juego sale de la consola y sus pkgs (juego, actualización y DLC) vuelven donde estaban, con el nombre que tenían. Antes se cierra el juego abierto. Lodestar deja de usar esos pkgs. No se borra nada.',
    'WARNING: this restores the original files of ALL {0} games: their pkgs (game, update and add-ons) go back where they were, with the names they had.\n\nThe open game is closed now, without saving. All Lodestar games leave the console, {1} at a time, and Lodestar stops using these pkgs. Nothing is deleted.\n\nContinue?':
      'ATENCIÓN: esto restaura los archivos originales de LOS {0} juegos: sus pkgs (juego, actualización y DLC) vuelven donde estaban, con el nombre que tenían.\n\nEl juego abierto se cierra ahora, sin guardar. Todos los juegos de Lodestar salen de la consola, de {1} en {1}, y Lodestar deja de usar esos pkgs. No se borra nada.\n\n¿Continuar?',
    'Restore the original files of {0} game(s), each with its update and all its add-ons?\n\nThe open game is closed now, without saving. The games leave the console{1}. Nothing is deleted.':
      '¿Restaurar los archivos originales de {0} juego(s), cada uno con su actualización y todos sus DLC?\n\nEl juego abierto se cierra ahora, sin guardar. Los juegos salen de la consola{1}. No se borra nada.',
    'Use the restored pkgs again? They are moved into the ps4 folder and added on the next scan.':
      '¿Volver a usar los pkgs restaurados? Se moverán a la carpeta ps4 y se añadirán en la próxima búsqueda.',
    "The console's": 'El de la consola',
    'Games removed from the library per group (1 to': 'Juegos que salen de la biblioteca por grupo (1 a',
    'Select all': 'Elegir todos',
    'Game': 'Juego',
    'Pick the pkgs on this computer. Each one is checked first (it takes a second): only pkgs this console can use are sent, so no transfer ends in a game that does not start. They are added to the library when they arrive.':
      'Elige los pkgs de este equipo. Cada uno se comprueba antes (tarda un segundo): solo se envían los que esta consola puede usar, así ningún envío acaba en un juego que no arranca. Se añaden a la biblioteca al llegar.',
    'Drop .pkg files here, or click to pick them': 'Suelta aquí los .pkg, o pulsa para elegirlos',
    'Send to': 'Enviar a',
    'Cancel transfer': 'Cancelar el envío',
    'Send': 'Enviar',
    '{0} of {1}': '{0} de {1}',
    'connection lost, trying again in {0} s': 'se cortó la conexión, reintento en {0} s',
    '{0} left': 'quedan {0}',
    'measuring the speed…': 'midiendo la velocidad…',
    'running for {0}': 'lleva {0}',
    'less than a minute': 'menos de un minuto',
    '{0} more waiting': '{0} más en espera',
    'Download': 'Descargar',
    /* states */
    'Ready': 'Listo',
    'Restart to play': 'Reinicia para jugar',
    'Running': 'Abierto',
    'Not in library': 'Fuera de la biblioteca',
    'On console storage': 'En el almacenamiento de la consola',
    'free': 'libre',
    'All platforms': 'Todas las plataformas',
    'PS4 games': 'Juegos de PS4',
    'Classics': 'Clásicos',
    'Homebrew': 'Homebrew',
    "What Lodestar keeps on the console's drive": 'Lo que Lodestar guarda en el disco de la consola',
    'Read only: this list measures, it deletes nothing. Lodestar cleans its own temporary files by itself and writes each one to the log.':
      'Solo lectura: esta lista mide, no borra nada. Lodestar limpia solo sus archivos temporales y deja cada uno en el log.',
    'Pkgs sent to install (deleted once installed)': 'Pkgs enviados para instalar (se borran al instalarse)',
    'Lodestar settings and records': 'Ajustes y registros de Lodestar',
    "Records of the library's games": 'Registros de los juegos de la biblioteca',
    "PlayGo records of the library's games": 'Registros PlayGo de los juegos de la biblioteca',
    "Icons and sounds of the library's games": 'Iconos y sonidos de los juegos de la biblioteca',
    'Pkgs in the internal library': 'Pkgs de la biblioteca interna',
    "{0} free of {1} on the console's drive": '{0} libres de {1} en el disco de la consola',
    'Could not read the storage': 'No se pudo leer el almacenamiento',
    'Where the games are': 'Dónde están los juegos',
    'Nothing is installed: each game plays straight from its .pkg files on a USB drive or the console\'s /data/PS4Mount when the internal-storage option is on. Lodestar only adds it to the home screen; the pkgs are never copied for it.':
      'No se instala nada: cada juego se juega directamente desde sus .pkg en un USB o el /data/PS4Mount de la consola cuando la opción de memoria interna está activada. Lodestar solo lo añade a la pantalla de inicio; los pkgs nunca se copian para ello.',
    'Add-ons come from the drive too: external mode mounts their packages from the source drive, console storage installs them on the console (up to 256 MB each, or any size with the big-add-ons option), off disables add-ons managed by Lodestar. Removing a game from the library, or restoring its files, never deletes a .pkg.':
      'Los DLC también vienen de la unidad: el modo externo monta los paquetes desde la unidad de origen, la memoria de la consola los instala en la consola (hasta 256 MB cada uno, o cualquier tamaño con la opción de DLC grandes) y apagado desactiva los DLC gestionados por Lodestar. Quitar un juego de la biblioteca, o devolver sus archivos, nunca borra un .pkg.',
    'Start with the console (GoldHEN AutoRun)': 'Arrancar con la consola (AutoRun de GoldHEN)',
    'Payload found: {0}. Enable Settings > GoldHEN > PayLoader > Enable AutoRun. Hold L1 at boot to skip AutoRun.':
      'Payload encontrado: {0}. Activa Ajustes > GoldHEN > PayLoader > Enable AutoRun. Mantén L1 al arrancar para saltar AutoRun.',
    'Payload file not found. Copy lodestar-ps4.bin to /data/payloads/ or /data/GoldHEN/payloads/ to enable AutoRun.':
      'No se encontró el archivo del payload. Copia lodestar-ps4.bin a /data/payloads/ o /data/GoldHEN/payloads/ para activar AutoRun.',
    'Adding': 'Añadiéndose',
    'On another drive': 'En otra unidad',
    'Drive not connected': 'Unidad no conectada',
    /* game cards and pages */
    'Add to library': 'Añadir a la biblioteca',
    'Remove from library': 'Quitar de la biblioteca',
    'Close the game to remove it': 'Cierra el juego para quitarlo',
    'Please wait…': 'Espera…',
    'Please wait...': 'Espera...',
    ' (with update)': ' (con actualización)',
    'mounted {0}': 'montado {0}',
    '1 add-on': '1 DLC',
    '{0} add-ons': '{0} DLC',
    'no add-ons': 'sin DLC',
    'Game{0}{1}': 'Juego{0}{1}',
    ', update': ', actualización',
    ' and {0}': ' y {0}',
    'Added. Restart the console once to play it.': 'Añadido. Reinicia la consola una vez para jugarlo.',
    'On {0}, which is not connected. Connect it to play.': 'En {0}, que no está conectado. Conéctalo para jugar.',
    '{0} game(s)': '{0} juego(s)',
    'State': 'Estado',
    'Version': 'Versión',
    'v{0} (game v{1} with its update)': 'v{0} (juego v{1} con su actualización)',
    'Add-ons': 'DLC',
    'none': 'ninguno',
    'Needs firmware': 'Necesita firmware',
    'unknown': 'desconocido',
    'Size': 'Tamaño',
    'On': 'En',
    'game': 'juego',
    'update': 'actualización',
    'add-on': 'DLC',
    'other': 'otro',
    /* messages */
    'Error: {0}': 'Error: {0}',
    'Ready to install':
      "Lista para instalar",
    "A homebrew app: it is sent to the console and installed with the system's installer, as by hand. The copy is deleted once it is installed.":
      "Una app homebrew: se envía a la consola y se instala con el instalador del sistema, como a mano. La copia se borra cuando queda instalada.",
    'Could not read the list: {0}': 'No se pudo leer la lista: {0}',
    'Could not read the settings: {0}': 'No se pudieron leer los ajustes: {0}',
    'Could not read the log: {0}': 'No se pudo leer el log: {0}',
    'Games per group must be a number from 1 to {0}': 'Los juegos por grupo deben ser un número de 1 a {0}',
    'Remove {0} from the library? Its pkgs stay where they are (deleting it on the console deletes them).':
      '¿Quitar {0} de la biblioteca? Sus pkgs se quedan donde están (borrarlo desde la consola sí los borra).',
    '{0} game(s) selected': '{0} juego(s) elegido(s)',
    ', removed from the library {0} at a time': ', salen de la biblioteca de {0} en {0}',
    'No games selected': 'No hay juegos elegidos',
    'No Lodestar games on the connected drives': 'No hay juegos de Lodestar en las unidades conectadas',
    ', {0} at a time': ', de {0} en {0}',
    '{0} free': '{0} libres',
    'No drive connected': 'No hay ninguna unidad conectada',
    'no place to send it to': 'no hay dónde enviarlo',
    'not enough space in {0}: {1} free, {2} needed': 'no hay espacio en {0}: {1} libres, {2} necesarios',
    'Checking…': 'Comprobando…',
    'Sending {0}%': 'Enviando {0}%',
    'Sent': 'Enviado',
    'Failed': 'Falló',
    'Cannot be used': 'No se puede usar',
    'Ready to send': 'Listo para enviar',
    'needs firmware {0}': 'necesita el firmware {0}',
    'Send again': 'Enviar de nuevo',
    'Remove': 'Quitar',
    'could not be checked: {0}': 'no se pudo comprobar: {0}',
    'the connection to the console was lost': 'se perdió la conexión con la consola',
    'cancelled': 'cancelado',
    'Cancel the transfer in progress?': '¿Cancelar el envío en curso?',
    'Stop Lodestar? The games stay unavailable until it is loaded again.':
      '¿Detener Lodestar? Los juegos no se podrán abrir hasta que se vuelva a cargar.',
    /* downloads from links */
    'Download links': 'Descargar enlaces',
    'The console downloads them itself (you can close this page): a pkg, a ZIP or a RAR, also in parts. Paste the direct links, the ones that start the download, one per line; all the parts of a RAR (name.part1.rar, name.part2.rar...) become one download. Its pkgs are added to the library when they arrive.':
      'La consola los descarga sola (puedes cerrar esta página): un pkg, un ZIP o un RAR, también en partes. Pega los enlaces directos, los que empiezan la descarga, uno por línea; todas las partes de un RAR (nombre.part1.rar, nombre.part2.rar...) son una sola descarga. Sus pkgs se añaden a la biblioteca al llegar.',
    'A link with no file name in it: add | and its name.': 'Un enlace sin nombre de archivo: añade | y su nombre.',
    'Password (if the archive has one)': 'Contraseña (si el archivo la tiene)',
    'Not needed for one that opened another download, nor for the site in brackets at the start of the name ([SITE]-game): those are tried by themselves.':
      'No hace falta si ya abrió otra descarga, ni si es el sitio entre corchetes al principio del nombre ([SITIO]-juego): esas se prueban solas.',
    'The password of the archive:': 'La contraseña del archivo comprimido:',
    'Save to': 'Guardar en',
    'Checking the links of {0}…': 'Comprobando los enlaces de {0}…',
    'through the browser': 'a través del navegador',
    'Downloading {0}': 'Descargando {0}',
    'Extracting {0}': 'Extrayendo {0}',
    'extracting {0}': 'extrayendo {0}',
    'downloading': 'descargando',
    'waiting': 'en espera',
    '{0} parts': '{0} partes',
    'Try again': 'Reintentar',
    'paste at least one link': 'pega al menos un enlace',
    'Remove this download? What it downloaded so far is deleted.': '¿Quitar esta descarga? Se borra lo que ya bajó.',
    'Cancel the download of {0}? What it downloaded so far is deleted.': '¿Cancelar la descarga de {0}? Se borra lo que ya bajó.',
    /* live activity, restart banner and notifications */
    'Scanning {0}': 'Buscando juegos en {0}',
    'Adding {0} of {1}: {2}': 'Añadiendo {0} de {1}: {2}',
    'Group {0}/{1}': 'Grupo {0}/{1}',
    'Waiting for PS4 system database…': 'Esperando la base de datos del sistema PS4…',
    'Restoring {0} of {1}: {2}': 'Restaurando {0} de {1}: {2}',
    'Installing add-ons…': 'Instalando DLC…',
    'Working…': 'Trabajando…',
    '1 game needs a one-time console restart to start: {0}':
      '1 juego necesita un reinicio único de la consola para empezar: {0}',
    '{0} games need a one-time console restart to start: {1}':
      '{0} juegos necesitan un reinicio único de la consola para empezar: {1}',
    ' +{0} more': ' +{0} más',
    'Show games': 'Mostrar juegos',
    'Show all': 'Mostrar todos',
    'Warning': 'Aviso',
    'Drive': 'Unidad',
    'Info': 'Info',
    'No notifications yet.': 'Aún no hay notificaciones.'
  }
};
var LANG = 'en';

/* The text in the panel's language; {0}, {1}... are replaced by the other arguments. */
function t(s) {
  var tab = LANGS[LANG], r = (tab && tab[s]) || s;
  for (var i = 1; i < arguments.length; i++) r = r.split('{' + (i - 1) + '}').join(arguments[i]);
  return r;
}

function known(k) {
  for (var l in LANGS) if (LANGS[l][k]) return true;
  return false;
}

/* The fixed texts of the page (text nodes and placeholder/title), by their English text,
 * kept on each node so the page can switch back. Generated parts use t() themselves. */
function translatePage() {
  var walker = document.createTreeWalker(document.body, 4, null, false), n, k, attrs, i, j, el, a;
  while ((n = walker.nextNode())) {
    if (/^(SCRIPT|STYLE|PRE|TEXTAREA)$/.test(n.parentNode.nodeName)) continue;
    if (n.psmEn === undefined) {
      k = n.nodeValue.replace(/\s+/g, ' ').replace(/^ | $/g, '');
      if (!k || !known(k)) continue; /* only texts some table knows */
      n.psmEn = k;
      n.psmLead = /^\s/.test(n.nodeValue) ? ' ' : '';
      n.psmTrail = /\s$/.test(n.nodeValue) ? ' ' : '';
    }
    n.nodeValue = n.psmLead + t(n.psmEn) + n.psmTrail;
  }
  attrs = ['placeholder', 'title'];
  for (i = 0; i < attrs.length; i++) {
    a = attrs[i];
    el = document.querySelectorAll('[' + a + ']');
    for (j = 0; j < el.length; j++) {
      if (!el[j].getAttribute('data-en-' + a)) el[j].setAttribute('data-en-' + a, el[j].getAttribute(a));
      el[j].setAttribute(a, t(el[j].getAttribute('data-en-' + a)));
    }
  }
}

/* Switches the page to tag ("es", "en"); English for a language with no table. */
function setLang(tag) {
  var code = LANGS[tag] ? tag : 'en';
  if (code === LANG) return false;
  LANG = code;
  document.documentElement.lang = code;
  translatePage();
  return true;
}
