# Émulateur minimal RX3 1.19 avec écran tactile virtuel

## Mode PC sans firmware privé

Pour travailler sur l'interface et les comportements des modules sans Docker,
QEMU ou les artefacts propriétaires, lancez :

```sh
make emulate-native
```

Ce mode est un modèle comportemental natif : deux decks, transport, grille de
beat-jump, key shift et toggles STEMS sont interactifs sur une fenêtre de PC.
Il n'exécute pas le binaire Pioneer et ne produit pas d'audio. Les tests de
firmware restent dans `make emulate`, qui exécute le véritable ARM `rbp` quand
le sysroot privé 1.19 est disponible.

Cet outil exécute le véritable binaire ARM `rbp` et les bibliothèques du
firmware 1.19 dans Docker/QEMU. Un shim remplace `fbdev` et les périphériques
indispensables au démarrage, puis exporte le framebuffer DirectFB en PNG. La
commande principale ouvre aussi une fenêtre 1280×720 : un clic y est routé
vers les onglets et les contrôles du broker KEY/STEMS.

Deux niveaux complémentaires sont disponibles :

- `make emulate` exécute `rbp`, affiche son framebuffer et fournit le tactile
  virtuel utile à la validation rapide des mods ;
- `make emulate-system` exécute le vrai U-Boot et le vrai noyau RX3 sur le
  modèle i.MX6Q de QEMU ;
- `make emulate-system-fast` poursuit avec le rootfs 1.19, `/sbin/init`,
  `rcS`, `apl_start`, `rbp` et vérifie que DirectFB crée son framebuffer.

Les deux chemins sont volontairement séparés :

```text
validation visuelle : Docker/QEMU-user -> rbp -> fbdev virtuel -> fenêtre tactile
validation système  : QEMU i.MX6Q -> noyau -> init -> apl_start -> rbp -> fbdev virtuel invité
```

Le framebuffer du second chemin est actuellement prouvé dans la VM mais pas
encore transporté vers la fenêtre macOS. Pour voir et piloter l'écran, utilisez
donc `make emulate`. Un succès graphique n'est pas présenté comme une preuve
du boot système, et réciproquement.

Le premier niveau ne simule pas un RX3 complet. Il valide le chargement ELF,
les gardes des hooks, le démarrage de `rbp`, le chemin de rendu natif et le
routage tactile des mods et le démarrage des bus PCM. Il ne valide pas encore
le DSP d'un morceau chargé, les LEDs, les microcontrôleurs de façade, les accès
USB réels ni la stabilité sur appareil.
Les contrôles stock hors du panneau Performance ne sont pas encore routés :
l'initialisation du vrai `TouchPanel` reste bloquée par des périphériques
absents sous QEMU.

## Prérequis

- Docker Desktop démarré avec l'émulation `linux/arm/v7` ;
- le sysroot 1.19 privé dans `local/research/rx3-lab/sysroot` ;
- `clang` et `lld` pour compiler le hook ARM lorsque des mods sont activés.

Le firmware, `rbp` et les ressources propriétaires restent dans `local/` et ne
sont jamais ajoutés au dépôt. Le sysroot est monté en lecture seule. Le runner
copie `/root/pdj` et met en cache `/root/gui` dans le conteneur avant
d'appliquer le patch requis. Cette copie locale des ressources GUI évite que
les lectures de `imagedata.dat` soient limitées par le partage de fichiers
Windows de Docker Desktop.

## Utilisation

```sh
make emulate
```

La commande teste le profil `all` pendant 60 secondes. Les résultats sont dans
`outputs/rx3-emulator/<date>/`. Cliquez dans l'écran pour piloter KEY/STEMS ;
Échap ou `q` ferme la session. Les artefacts comprennent :

- `framebuffer.png` : dernière image 1280×720 ;
- `audio-playback-0.wav` à `audio-playback-2.wav` : les trois bus de sortie
  natifs, convertis en WAV stéréo 24 bits à 44,1 kHz ;
- `rbp.log` et `hook.log` : journaux séparés ;
- `report.json` : empreintes des binaires, assertions et périmètre de preuve.

Pour isoler une régression :

```sh
python3 -m tools.rx3_emulator.cli --profile stock --duration 45
python3 -m tools.rx3_emulator.cli --profile keyshift --duration 60
python3 -m tools.rx3_emulator.cli --profile stems --duration 60
python3 -m tools.rx3_emulator.cli --profile all --duration 60
```

To run against a real Rekordbox-exported USB folder, pass its root directory:

```sh
python3 -m tools.rx3_emulator.cli --profile all --media /path/to/USB --duration 120
```

The runner presents that folder at the firmware's native `/media/usb1/sda1` path
and writes `media.json` with the detected `PIONEER/rekordbox/export.pdb`,
`exportExt.pdb`, and `PIONEER/LIBRARY/PDTL.DB` files. This is the bridge for
the next playback milestone; a plain audio folder is not a Rekordbox library.

When media is present, the modified profile also reproduces the RX3 kernel's
queued `/proc/udev_usb1` mount event, calls the genuine
`UsbMountManager::force_mount` method, and dispatches the native USB1 and
BROWSE keys on the UI thread. In the reduced PC startup, the hardware-backed
`UsbStorageManager` is absent, so the emulator bridges the mounted path through
the firmware VFS and legacy `DBC_DriveAnalysisStart` entry point. Calling the
modern `DbProxy` here is unsafe because its asynchronous completion expects the
missing storage manager. The runner verifies the VFS bridge by opening
`B:/PIONEER/rekordbox/export.pdb` through the firmware's own database wrapper,
then commits the USB1/category state that the missing R232C consumer would
normally apply. These steps are reported separately.

This path now reaches the genuine browser view headed `READ ONLY (USB1)` and
reports browse device 3 / mode 3. Empty rows are not treated as successful
track enumeration: populated category/track data and track loading remain the
next milestone.

Un succès du profil modifié exige un framebuffer non vide, le fichier de
readiness du hook, le message d'activation, la table d'images privée, des
compteurs de rendu non nuls et le canal tactile virtuel.

Le profil `all` 1.19 a été validé sur PC avec le véritable écran rekordbox
1280×720. Les onglets KEY/STEMS et leurs contrôles sont dessinés par le chemin
DirectFB natif ; un clic STEMS puis un clic de contrôle produisent un redraw et
les événements correspondants dans `hook.log`. Prévoyez environ quatre minutes
pour une première exécution sous QEMU sur Docker Desktop : l'installation des
hooks ARM et le chargement des ressources sont volontairement privilégiés à la
vitesse de démarrage.

Le backend audio PC présente aussi au firmware l'identité de carte rev-7 et
les noms ALSA qu'il attend. L'initialisation passe par le véritable singleton
`DjEngineIF`, déclenche `audioDeviceAboutToStart` et cadence les trois bus PCM.
Les captures WAV prouvent le moteur et le routage audio ; le chargement d'un
morceau reste séparé, car `StTrackInfo` contient des métadonnées issues de la
base Rekordbox qui ne peuvent pas être remplacées par un simple chemin de
fichier.

Deux ELF sont compilés. `librx3_core.so` reste le livrable de production.
`librx3_core_emulator.so` ajoute uniquement, sous `RX3_EMULATOR_BUILD`, le
panneau initial et le canal de clics nécessaires faute de façade physique. Les
deux empreintes sont consignées dans `report.json`. L'acceptation finale d'un
mod audio ou matériel reste un test sur RX3 réel. Les périphériques ne se
laissent pas convaincre par un PNG, ce qui est d'une mesquinerie assez
constante chez eux.

## Émulation système i.MX6Q

```sh
make emulate-system
```

Cette commande utilise directement les artefacts privés 1.19 :
`u-boot.bin.nand`, `uImage` et `rootfs.cramfs`. Leurs SHA-256 sont contrôlés
avant exécution. Deux journaux et un rapport JSON sont écrits dans
`outputs/rx3-system-emulator/<date>/`.

Le U-Boot Pioneer est chargé à son adresse de link `0x17800000`. Le noyau
Linux 3.0.101 est inchangé ; un petit stub fournit uniquement le
protocole ATAG historique, l'identifiant machine Sabre-SD `3980` et l'adresse
d'entrée `0x10008000`. Cette adaptation est requise parce que la machine QEMU
`sabrelite` démarre normalement avec un device tree, alors que le noyau RX3 a
`CONFIG_USE_OF` désactivé.

La sonde validée atteint actuellement :

- U-Boot 2009.08, i.MX6Q, 1 Gio de RAM, I²C et quatre contrôleurs USDHC ;
- Linux `3.0.101-2790-gc248ed7-svn3098` ;
- les pilotes RX3 `subucom`, `gpiodrv`, `TSC2007` et `mxc_sdc_fb`.

Pour démarrer également l'environnement utilisateur réel :

```sh
make emulate-system-fast
```

Ce profil extrait l'`Image` ARM du `uImage`, superpose le sysroot privé dans un
initramfs externe et adapte seulement les interfaces matérielles absentes. Les
ELF remplacés par un wrapper restent présents avec le suffixe `.real`. La
chaîne vérifiée est :

```text
Linux 3.0.101 -> /init -> BusyBox init -> rcS -> rc.local
              -> apl_start -> rbp.real -r -a -> framebuffer 1280x720x32
```

Une seule instruction noyau est neutralisée : l'entrée `gpu_init` retourne
zéro, car QEMU ne fournit aucun modèle Vivante Galcore. La garde vérifie les
8 octets d'origine avant le patch et le rapport consigne avant/après, offset,
empreintes du firmware, de l'initramfs et du shim. Les variables U-Boot,
`bootmod`, `boardrev`, GPIO et registres sont simulés uniquement dans
l'initramfs temporaire ; le sysroot privé sur disque n'est pas modifié.

Ce profil ne fait pas passer le noyau à travers U-Boot : les deux binaires
Pioneer sont exécutés et vérifiés par des sondes distinctes. Les contrôleurs
de stockage attendus par le script de boot original ne sont pas suffisamment
modélisés par la machine `sabrelite` pour former une chaîne U-Boot → SD/NAND
fidèle.

Le boot système complet exige encore des modèles QEMU pour GPMI NAND/APBH
DMA, IPU/LDB, le tactile I²C/IRQ et les coprocesseurs Pioneer SPI. Le modèle
i.MX6Q amont fournit le CPU, les UART, timers, GPIO, I²C, USDHC, USB, SPI et
Ethernet, mais pas la NAND ni le pipeline graphique utilisés par ce firmware.
En conséquence, l'écran tactile exploitable reste pour l'instant fourni par
`make emulate`; les modes système prouvent séparément le bootloader, le SoC,
le noyau et le démarrage de l'espace utilisateur sans prétendre simuler les
périphériques manquants.
