[English](README.md) · **Français**

# Bus CIAT Micro-Connect ↔ ESPHome

Rétro-ingénierie du bus 2 fils entre le terminal d'ambiance **CIAT Micro-Connect** et la pompe à chaleur
**CIAT Aqualis 2**, et un **ESP32 (ESPHome) qui remplace le terminal** pour surveiller et piloter la PAC
depuis **Home Assistant**.

> ⚠️ **La sécurité d'abord : à lire avant de brancher quoi que ce soit.**
> Les deux mêmes fils portent l'alimentation et les données, et le retour du bus est **relié à la terre dans
> la PAC**. **Ne reliez jamais la masse d'un appareil relié à la terre** (oscilloscope, PC, chargeur secteur)
> **à un fil du bus** : le 15/11/2026, cela a mis la PAC en défaut, et il a fallu couper l'alimentation du
> groupe extérieur pour la relancer.
> L'ESP32 doit rester **isolé galvaniquement** du bus (optocoupleurs en réception et en émission).
> **N'alimentez jamais l'ESP32 par le bus.** Le terminal d'origine ne consomme que **~1,5 mA** en moyenne
> (ondulation de 1 à 5 mA, mesurée avec une résistance de shunt dans un fil du bus) ; un ESP32 demande ~80 mA, avec
> des pointes à 300 mA en Wi-Fi. Utilisez une alimentation USB séparée.
> Vous intervenez sur votre matériel à vos risques ; voir l'avertissement plus bas.

## Ce qui fonctionne

| | État |
|---|---|
| Protocole du bus : UART 9600 8N1, XOR de contrôle, cycle d'interrogation de ~220 ms, toutes les trames décodées | ✅ |
| Carte complète des paramètres P00 à P99 (réglages et mesures) | ✅ |
| Écoute passive par un optocoupleur HCPL-0501, dans les deux sens | ✅ en service depuis novembre 2026 |
| Étage d'émission isolé, qui répond à la PAC à la place du terminal | ✅ en service depuis le 23/11/2026, terminal débranché |
| Entités `climate` de Home Assistant : chauffage (presets confort / éco / absent, consigne) et piscine | ✅ validées sur la PAC |
| Lecture automatique des mesures (températures d'eau, extérieure, fréon…) et des réglages par le menu du terminal | ✅ |
| Planning hebdomadaire dans Home Assistant (remplace le programme du terminal) | ✅ |
| Écriture des paramètres depuis Home Assistant | 🟡 seulement en « mode secours », bas niveau |
| Froid, appoints / chaudière, eau chaude, codes défaut, délestage | ❓ jamais observés sur l'installation de référence ([protocole §9](docs/protocol.fr.md#9-pas-encore-observé)) |

Chaque constat de la documentation est marqué ✅ confirmé, 🟡 hypothèse ou ❓ inconnu, avec sa preuve.

## Le protocole en bref

- UART 9600 bauds, 8N1. Le dernier octet de chaque trame est le **XOR** des précédents.
- Bus à collecteur ouvert : la PAC et le terminal tirent tous deux la ligne vers 0 V pour émettre. Une seule
  entrée sur la tension du bus voit donc les deux sens.
- La PAC est maître. Elle répète ce cycle toutes les ~220 ms :

| Trame | Sens | Octets | Contenu |
|---|---|---|---|
| Q14 | PAC → terminal | `14 14` | requête « horloge » |
| 14.1 | terminal → PAC | `EE jj HH MM xx` | jour, heure, minutes (BCD) |
| Q12 | PAC → terminal | `12 12` | requête « état » |
| 12.1 | terminal → PAC | `AC b1 b2 TT Fd 12 xx` | mode demandé, molette et touches, température ambiante (BCD) |
| 12.2 | PAC → terminal | `40 p1 p2 d1 d2 d3 d4 xx` | pictogrammes, compresseur, les 4 chiffres 7 segments |

- Le **terminal** gère l'horloge, le programme hebdomadaire, la sonde d'ambiance et l'état brut des touches
  et de la molette.
- La **PAC** gère les consignes, les paramètres P00 à P99 (dont la loi d'eau) et tout ce qu'affichent ses
  4 chiffres.

## Documentation

- [Protocole](docs/protocol.fr.md) : couche physique, trames, carte des registres, comportement du menu, paramètres.
- [Matériel](docs/hardware.fr.md) : l'interface ESP32 isolée (réception et émission), essai sur table, branchement.
- [Émulateur](docs/emulator.fr.md) : installation, fonctionnement, thermostats, sécurité, diagnostic.
- [Home Assistant](docs/home-assistant.fr.md) : thermostats, planning de la semaine, notifications, tableau de bord.

## Démarrage rapide

1. Lisez l'avertissement de sécurité ci-dessus et la page [matériel](docs/hardware.fr.md) ; montez l'interface.
2. Essayez l'étage d'émission sur table avec `esphome/ciat-bench.yaml`.
3. Flashez `esphome/ciat-emulator.yaml` en écoute seule, terminal d'origine toujours branché, et vérifiez le décodage
   dans Home Assistant.
4. Passez en émission, débranchez le terminal, et pilotez la PAC depuis Home Assistant
   ([émulateur](docs/emulator.fr.md)).

## Organisation du dépôt

```
docs/                  protocole, matériel, émulateur, Home Assistant (anglais + français)
hardware/              schéma de l'interface (SVG), photos du terminal d'origine
esphome/               firmware ESPHome : émulateur (aussi en écoute seule) et essai sur table
tools/                 tests PC de la logique du firmware, décodeur des captures brutes
examples/              vue de tableau de bord, automatisation de notifications, paquet de puissance calorifique pour Home Assistant
captures/              six captures brutes à l'oscilloscope et le tableau décodé de toute la campagne
```

Lancer les tests : `g++ -std=c++17 -I esphome tools/test_ciat_nav.cpp -o /tmp/tn && /tmp/tn` (idem pour
`test_ciat_emul.cpp` ; le test de la tâche du bus a besoin en plus des fonctions ESP-IDF simulées : ajouter
`-I tools/stub_idf` pour `test_ciat_link.cpp`).

## Avertissement

Projet indépendant, **ni affilié à CIAT ni approuvé par CIAT**. CIAT, Aqualis et Micro-Connect sont des
marques de leurs propriétaires respectifs, citées ici seulement pour identifier le matériel. Les informations
viennent de l'observation d'une seule installation : d'autres versions de firmware peuvent se comporter
autrement. Rien n'est garanti : modifier son installation de chauffage peut faire perdre la garantie et se
fait entièrement à vos risques.

## Licence

- Code (firmware, outils, tests) : [MIT](LICENSE).
- Documentation, schémas et photos : [CC BY-SA 4.0](LICENSE-docs.md).
