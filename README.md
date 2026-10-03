# emugcxbox360

Émulateur GameCube pour Xbox 360 modée (RGH/JTAG), lancé depuis XeLL avec libxenon.
Le cœur est portable : il se compile aussi sur Linux (frontend SDL2) pour développer et tester sans console.

Licence : GPL-2.0-or-later. Le code est écrit pour ce projet en s'appuyant sur la documentation publique du matériel (YAGCD) et sur le comportement de Dolphin (GPL), dont il peut reprendre du code à l'avenir.

## État

| Bloc | État |
|---|---|
| CPU Gekko | Interpréteur complet (entiers, branches, load/store, FPU, paired singles, `psq_l/st`, exceptions, décrémenteur, timebase, cache verrouillé) + cache de blocs pré-décodés + saut des boucles d'attente (idle skipping). **JIT PowerPC → PowerPC** sur hôte PowerPC (360) : instructions entières, mémoire, FPU et paired singles en code natif, registres en cache, dispatcheur généré ; vérifié identique au bit près à l'interpréteur sous qemu-ppc |
| Mémoire | MEM1 24 Mo, traduction BAT, cache L2 verrouillé, MMIO, write-gather pipe, accès CPU à l'EFB |
| VI | Compteur de lignes, interruptions d'affichage, état laissé par le BIOS (PAL/NTSC selon le disque), scan-out XFB |
| SI | Manette standard |
| EXI | IPL (RTC + SRAM avec flash ID), **carte mémoire slot A** (image `.raw` de 59 blocs créée et formatée automatiquement) |
| DI | Lecture disque. Formats : ISO/GCM, RVZ et WIA de Dolphin (zstd ou sans compression ; GameCube uniquement) |
| DSP | Microcodes en HLE : ROM, INIT, CARD, GBA, **Zelda** (son complet : AFC/PCM, filtres, Dolby, réverb) et **AX** (son complet : ADPCM, rééchantillonnage, bus AUX, compresseur) |
| GX (GPU) | **Rendu sur le GPU Xenos** : le TEV est traduit en pixel shaders générés à la volée (microcode Xenos assemblé par l'émulateur), EFB en EDRAM, copies XFB présentées directement ; vérifié sur PC par un simulateur du microcode (`--gpu-sim`) |
| GX (logiciel) | **Rendu 3D logiciel complet** : chargeur de sommets (tous formats), transformation + éclairage 8 lumières, génération de coordonnées, clipping, rasterisation perspective, TEV 16 étages (indirect, fog, z-texture), tous les formats de texture, mipmaps, EFB (blend, logic ops, formats), copies vers XFB et vers textures. Rasterisation multi-thread |
| Divers | Save states (déterministes au bit près), HLE de fonctions (OSReport) |

Testé avec *The Legend of Zelda: The Wind Waker* (PAL, RVZ) : écran titre rendu correctement avec sa musique, création de sauvegarde sur la carte mémoire, menu de sélection de fichier.

Limites actuelles : le rendu GPU et le JIT n'ont pas encore été validés sur la console (mipmaps absents en mode GPU), pas de MMU par tables de pages (quelques jeux Star Wars), son AX non testé faute de jeu AX disponible.

## Compiler sur Linux (SDL2)

```bash
sudo apt install build-essential cmake libsdl2-dev
cmake -B build && cmake --build build -j
./build/emugc_tests                 # tests unitaires du CPU
python3 tests/make_test_dol.py build/test.dol
./build/emugc build/test.dol        # barres de couleur ; X = bouton A, flèches = stick
```

Pour vérifier une image disque (format, ID du jeu, CRC32/SHA-1 à comparer avec redump.org, conversion optionnelle en ISO) :

```bash
./build/emugc_disctool jeu.rvz [sortie.iso]
```

Les RVZ compressés en bzip2/LZMA (option non standard de Dolphin) ne sont pas gérés : reconvertis-les en zstd ou en ISO avec `dolphin-tool convert`.

Options utiles : `--gpu-sim` (rendu par la traduction Xenos + simulateur de microcode), `--headless`, `--frames N`, `--dump image.ppm` (`--dump-every N`), `--wav son.wav`, `--memcard fichier.raw|none`, `--save-state N:fichier`, `--load-state fichier`, `--input 1000:start:10,1400:sleft:20` (entrées scriptées), `--threads N`, `--stats`, `--osreport ADDR`, `--interpreter`, `--unthrottled`, `--scale N`. Touches F1/F3 : sauvegarde/chargement rapide.

Clavier → manette GameCube : X=A, Z=B, C=X, S=Y, Entrée=Start, D=Z, Q/W=L/R, flèches=stick, IJKL=C-stick, TFGH=croix. Une manette Xbox branchée en USB est aussi reconnue.

## Compiler pour Xbox 360

1. Installer la toolchain libxenon (GCC 9.4 + newlib, sans sudo) :
   ```bash
   git clone https://github.com/gligli/libxenon ~/xenon-src/libxenon
   cd ~/xenon-src/libxenon/toolchain
   export PREFIX=$HOME/xenon DEVKITXENON=$HOME/xenon PATH=$PATH:$HOME/xenon/bin:$HOME/xenon/usr/bin
   ./build-xenon-toolchain toolchain     # binutils + GCC 9.4 + newlib (~30-60 min)
   ./build-xenon-toolchain libxenon
   ./build-xenon-toolchain bin2s
   ./build-xenon-toolchain filesystems   # libfat (clés USB), ext2, ntfs, xtaf
   ```
   L'étape `libs` du script échoue actuellement (zlib 1.2.11 n'est plus téléchargeable sur zlib.net). L'émulateur n'en a pas besoin : `filesystems` suffit.
2. Compiler :
   ```bash
   export DEVKITXENON=$HOME/xenon
   export PATH=$PATH:$DEVKITXENON/bin:$DEVKITXENON/usr/bin
   make -f Makefile.xenon
   ```
3. Copier `emugcxbox360.elf32` sur une clé USB (FAT32) avec tes `.dol`/`.iso`/`.gcm`/`.rvz` à la racine ou dans `/gc/`, puis lancer l'elf depuis XeLL.

Sur la 360, dans le menu : stick/croix pour choisir, A pour lancer, et ces réglages (tous actifs par défaut sauf X) :
- **Y** : affichage par le GPU ; **B** : rendu par le GPU (sinon rendu logiciel) ;
- **Start** : recompilateur JIT (sinon interpréteur) ;
- **X** : rendu logiciel multi-thread (expérimental) ;
- bouton Guide : retour à XeLL.

Mapping en jeu : A=A, X=B, B=X, Y=Y, RB=Z, gâchettes=L/R. La carte mémoire `memcard_a.raw` est créée à côté du jeu. En haut à gauche : FPS et vitesse (%), puis la répartition du temps (C = CPU émulé, G = commandes GPU/copies EFB, A = audio, V = affichage), aussi sur le port série.

## Organisation

```
src/core/            cœur portable (aucune dépendance plateforme)
  gekko/             CPU : état, boucle, interpréteur (tables, entiers, load/store, FPU + paired singles)
  hw/                Flipper : PI, MI, VI, SI, EXI, DI, DSP/ARAM/AI, CP/PE/GX
  loader/            DOL + boot HLE (IPL/apploader)
  memory.*           mémoire et traduction d'adresses
  coretiming.*       ordonnanceur d'événements en cycles CPU
  system.*           API utilisée par les frontends
src/platform/        platform.h (interface Host), sdl/ (PC), xenon/ (Xbox 360)
  disc/              lecteurs d'images : ISO, WIA/RVZ (zstd + padding RVZ)
third_party/zstd/    décodeur Zstandard 1.5.6 (fichier unique, licence BSD)
  jit/               recompilateur PowerPC -> PowerPC (émetteur, blocs, dispatcheur)
src/xenos/           assembleur de microcode Xenos, traduction GX -> shaders/états, simulateur
tests/               tests unitaires CPU, générateurs de DOL de test, emugc_disctool,
  headless_main.cpp  hôte sans fenêtre (empreintes image/son/RAM par trame)
  ppc_linux/         build PowerPC big-endian du cœur pour qemu-ppc (vérification sans console)
tools/xenos_disasm.py  désassembleur de microcode Xenos
docs/PLAN.md         plan de travail en cours
```

## Feuille de route

Voir [docs/PLAN.md](docs/PLAN.md). Prochaines étapes : validation sur la console du rendu GPU et du JIT, mipmaps en mode GPU, copies EFB → texture gardées sur le GPU, multi-cœur.

## Vérifier sans console

```bash
make -f tests/ppc_linux/Makefile EXTRA="-ffp-contract=off -DEMUGC_NO_FMA"
qemu-ppc -cpu 7450 build_ppc/emugc_headless jeu.rvz 300                 # avec JIT
EMUGC_JIT=0 qemu-ppc -cpu 7450 build_ppc/emugc_headless jeu.rvz 300     # interpréteur
```
Les deux sorties (empreintes par trame) doivent être identiques.
