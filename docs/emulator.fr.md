[English](emulator.md) · **Français**

# L'émulateur ESPHome

`esphome/ciat-emulator.yaml` fait d'un ESP32-C3 un **terminal Micro-Connect virtuel**. Il répond à la PAC exactement
comme le terminal d'origine, et présente la PAC à Home Assistant sous forme de thermostats et de capteurs.

C'est un émulateur « à moitié » : il parle au bus comme le terminal, mais n'expose pas de touches virtuelles à
l'utilisateur. Un navigateur traduit des demandes de haut niveau (« preset éco », « consigne 21,5 », « lire P40 ») en
appuis de touches, et **vérifie chaque résultat sur l'afficheur de la PAC** avant de continuer.

## 1. Installation

1. Montez l'interface et **essayez l'étage d'émission sur table** ([matériel](hardware.fr.md), `esphome/ciat-bench.yaml`).
2. Copiez les fichiers de `esphome/` dans votre dossier de configuration ESPHome :
   `ciat-emulator.yaml`, `ciat_bus.h`, `ciat_emul.h`, `ciat_link.h`, `ciat_nav.h`, `ciat_flash.h`.
3. Ajoutez à votre `secrets.yaml` : `wifi_ssid`, `wifi_password`, `ciat_api_key` (32 octets en base64, par exemple
   `openssl rand -base64 32`), `ciat_ota_password`.
4. Dans `substitutions`, mettez dans `room_sensor` l'entité Home Assistant d'un thermomètre d'ambiance. Il remplace
   la sonde du terminal (la PAC s'en sert pour la compensation d'ambiance).
5. **Premier essai, en écoute seule** : gardez `tx_compiled: "false"` et le terminal d'origine branché. Vérifiez
   dans Home Assistant que « Bus active » est allumé, que « Valid frames per minute » est à ~1 300 et « Rejected
   bytes per minute » à ~0, et que « Shadow differences per minute » reste à 0 (la 12.1 de l'émulateur serait
   identique à celle du terminal).
6. **Remplacer le terminal** : mettez `tx_compiled: "true"`, flashez, débranchez le terminal d'origine, mettez
   « Bus transmission » en marche. La PAC garde son état ; les thermostats apparaissent en quelques secondes.

Essayé avec ESPHome 2026.11 (framework ESP-IDF) sur un ESP32-C3 Super Mini.

Les noms des entités sont en anglais dans le firmware ; vous pouvez les renommer dans Home Assistant.

## 2. Qui fait quoi

| Couche | Fichier | Rôle |
|---|---|---|
| Tâche du bus | `ciat_link.h` | tâche FreeRTOS : lit l'UART, répond à chaque requête **10,5 ms** après son début, vérifie l'écho. Indépendante de la boucle ESPHome et du réseau |
| Trames | `ciat_emul.h` | construit les réponses 14.1 (horloge) et 12.1 (état) |
| Décodeur | `ciat_bus.h` | découpe le flux en trames (en-tête + longueur + XOR + contenu), les décode pour Home Assistant |
| Navigateur | `ciat_nav.h` | pilote le menu des paramètres et la consigne par appuis de touches, relit chaque résultat |
| Sauvegarde | `ciat_flash.h` | garde en flash les réglages relevés |
| Liaison | `ciat-emulator.yaml` | entités ESPHome, thermostats, scans, garde-fous |

Les parties en logique pure (`ciat_emul.h`, `ciat_nav.h`) sont testées sur PC : `tools/test_ciat_emul.cpp` (trames
réelles) et `tools/test_ciat_nav.cpp` (un simulateur du menu de la PAC, 46 cas).

## 3. Thermostats

### Chauffage (`climate.<nom>_heating`)

| Thermostat | PAC |
|---|---|
| mode chauffe / arrêt | molette sur chaud / arrêt |
| preset confort / éco | molette sur chaud + créneau du programme envoyé dans b1 |
| preset absent | molette sur hors-gel |
| consigne | réglée hors menu par + et −, 15 à 30 °C par 0,5 |
| température actuelle | l'ambiante envoyée à la PAC |
| action chauffe / repos | bit compresseur |

- L'état affiché vient toujours de la PAC (`optimistic: false`) : Home Assistant ne montre jamais ce que la PAC n'a
  pas affiché.
- Changer la consigne **change la consigne confort ou éco de la PAC elle-même**, de façon durable (c'est la
  consigne du créneau en cours).
- **Le froid n'est pas géré** : les positions froid et programme froid de la molette n'ont jamais été observées
  (protocole §9).
- Le programme hebdomadaire du terminal d'origine n'existe plus : **le créneau vient de Home Assistant**. N'importe
  quel planning qui règle le preset convient (voir [Home Assistant](home-assistant.fr.md)).

### Piscine (`climate.<nom>_pool`)

- Chauffe = molette sur piscine ; arrêt = molette sur arrêt (seulement si la molette était sur piscine).
- Consigne = consigne piscine (P23), 20 à 35 °C par 0,5 : la plage de la fiche CIAT ; la PAC a accepté les deux
  bornes ✅.
- Température actuelle = P50 (lue dans les scans du menu, seulement molette sur piscine).
- La PAC ne chauffe ensuite que si son **contact piscine** est fermé (câblé par l'installateur, en général par la
  pompe de la piscine).

### Application des demandes

- Les demandes sont mises en attente et appliquées quand le navigateur est libre. Une demande **interrompt un scan
  des mesures** (sortie propre du menu, ~3 s), jamais une écriture de paramètre.
- Une demande qui ne change rien (molette déjà en place) est ignorée : un planning peut la répéter sans risque.
- La consigne est atteinte par appuis + et − de **3 trames** chacun (hors menu, la PAC ignore les appuis plus
  courts), relus un par un, puis confirmée pendant ~2 s. Une seule tentative ; une erreur est publiée dans
  « Navigator last error ».

## 4. Mesures et réglages

- **Scan des mesures** (P40-P48, plus P50 molette sur piscine) : toutes les « Measurement scan period » (5 min par
  défaut, 2 min convient bien), au niveau installateur. ~32 s par scan : ~8 s pour entrer au niveau installateur et
  ~3 s pour sortir sont imposées par la PAC. Chaque valeur est moyennée sur 5 cycles (P50 : 9).
- **Scan des réglages** (P00-P36, P49, P60-P63, P80-P99) : au démarrage puis une fois par jour. Résultats dans
  « Settings P00 to P29 » et « Settings P30 to P99 », gardés en flash.
- Les scans ne tournent qu'avec « Automatic scan » en marche, l'émission active, et la molette sur une position qui
  autorise le menu (froid, arrêt, hors-gel, chaud, piscine).
- La PAC **continue de réguler pendant le menu** ; un changement de créneau qui arrive pendant un scan est appliqué
  à la sortie du menu.
- Relevé passif : avec le terminal d'origine en écoute seule, les valeurs consultées à la main dans son menu sont
  publiées aussi.

## 5. Sécurité et robustesse

| Situation | Comportement |
|---|---|
| ESP32 éteint, qui redémarre ou planté | optocoupleur d'émission éteint : **bus libre**. La PAC continue sur son dernier état |
| Redémarrage | le choix « Bus transmission » est gardé en flash ; après 20 s d'écoute, l'émission reprend, sauf si un autre terminal répond ou si le bus est muet |
| Home Assistant ou Wi-Fi perdus | pas de redémarrage (`reboot_timeout: 0s`) ; les réponses continuent avec la dernière molette, le dernier créneau et la dernière ambiante (gardée en flash) |
| Autre terminal sur le bus | 3 trames étrangères en 10 s → émission inhibée |
| Collision (écho différent ou absent) | inhibée tout de suite |
| La PAC ne répond plus à nos trames | 20 cycles sans 12.2 → inhibée |
| Inhibition | reprise automatique au bout de 30 s si aucun autre terminal n'est vu, **au plus 3 fois par heure** ; ensuite elle reste coupée |
| Réponse trop tardive (> 16 ms) | abandonnée, la PAC passe à la suite |
| Menu laissé ouvert (redémarrage pendant un scan) | refermé par un OK long au bout de 3 s |

⚠️ Après une coupure de courant, la PAC **ne démarre pas tant qu'aucun terminal ne répond**. Garder la dernière
ambiante en flash garantit que l'ESP32 le peut toujours, même sans Home Assistant.

Limites qui restent sans Home Assistant : pas de planning (preset figé), pas d'heure après un redémarrage (pas de
14.1 ; effet sur la PAC ❓), pas de thermostats.

## 6. Mode secours

« Rescue mode » (configuration) débloque les commandes bas niveau pendant 30 minutes : touches + / − / OK, entrée et
sortie du menu, écriture d'un paramètre (liste blanche seulement, valeur relue), scans manuels, réinitialisation du
navigateur. Sans lui, ces boutons ne font rien et le signalent dans « Navigator last error ». À utiliser pour changer
un réglage que les thermostats ne couvrent pas, ou pour débloquer le navigateur.

Pour réarmer un **défaut permanent** (protocole §8.2 : molette sur arrêt, OK tenu 3 s) : mettez le thermostat de
chauffage sur arrêt, puis appuyez sur « Long OK (leave the menu) » (OK tenu ~4 s) en mode secours. 🟡 Jamais essayé :
aucun défaut ne s'est produit sur l'installation de référence.

## 7. Diagnostic

| Entité | Usage |
|---|---|
| Emulator state | `transmission active`, `inhibited: <cause>`, `ready, transmission off`, `(room temperature frozen …)` |
| Last bus anomaly | chaque trame étrangère, écho différent ou absent : heure, octets, délai après la requête, écho attendu ou non, trame envoyée |
| Wrong echoes / Dropped answers / Answers sent per minute | santé de l'émission (~530 réponses par minute) |
| Navigator state, last error, last scan, last write | ce que fait le navigateur et comment ça s'est terminé |
| Raw heat-pump / terminal frame, Display, Dial, Keys | décodage en direct |
| Last key press | chaque appui : touches, nombre de trames, réaction de la PAC et délai |
| New heat-pump frame / Single-cycle heat-pump frame | trames jamais vues depuis le démarrage, et trames d'un seul cycle |
| Last reboot reason, Uptime | coupure d'alimentation ou plantage |

Une automatisation Home Assistant sur « Last bus anomaly » donne une notification avec tout le diagnostic à chaque
incident (voir `examples/`).
