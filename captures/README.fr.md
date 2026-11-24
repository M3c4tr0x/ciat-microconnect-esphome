[English](README.md) · **Français**

# Captures du bus

Enregistrements bruts du bus CIAT faits à l'oscilloscope (Siglent SDS1104X-E, 2,5 Méch/s) sur l'installation de
référence, terminal d'origine branché.

## Formes d'onde brutes (`*.npz`)

Six captures représentatives, ~4 Mo chacune (NumPy `savez_compressed`) :

| Fichier | Contenu |
|---|---|
| `F01_1.npz` | mise sous tension du terminal : première 12.1 avec b1 = `7E`, puis cycles normaux |
| `C04_1.npz` | un appui sur + hors menu : consigne 15.0c → 15.5c |
| `Z05_1.npz` | molette passée d'arrêt à hors-gel : `----`, puis `10.0c` ~1,1 s plus tard |
| `J05_1.npz` | menu installateur : P80 ouvert (version de la carte `9.00`) |
| `K03_1.npz` | OK appuyé une fois en programmé : confort / éco forcé |
| `R05_1.npz` | P50 affiché en direct dans le menu : la valeur change à chaque cycle |

Chaque fichier contient `c2` (tension entre les fils du bus, les deux sens), `c1` (une sonde sur le transistor
d'émission du terminal d'origine : trames du terminal seulement) et `meta` (JSON : essai, action, réglages de
l'oscilloscope, `t0` = instant du premier échantillon).

Pour en décoder une : `python tools/decode_capture.py captures/F01_1.npz` (il faut `numpy`). Une trame coupée au tout
début d'un enregistrement apparaît en « bad XOR ».

⚠️ Ces captures ont été faites avec la masse de l'oscilloscope sur le « − » du terminal, derrière son pont de diodes.
Ça a été toléré, mais **ne mettez jamais une masse reliée à la terre sur un fil du bus** (voir
[protocole §1](../docs/protocol.fr.md#1-couche-physique)).

## Tableau décodé (`frames.csv`)

Une ligne par capture de toute la campagne (234 captures) : les premières trames 14.1, 12.1 et 12.2, leurs champs
décodés, et la note prise sur le moment (`note_fr`, en français).
