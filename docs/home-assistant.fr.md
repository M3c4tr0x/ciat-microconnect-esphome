[English](home-assistant.md) · **Français**

# Intégration Home Assistant

L'émulateur apparaît dans Home Assistant comme un appareil ESPHome (« CIAT heat pump » par défaut), avec deux
thermostats, les mesures de la PAC et des diagnostics. Exemples dans
[`examples/home-assistant/`](../examples/home-assistant/).

Les identifiants d'entités ci-dessous supposent les noms par défaut ; vérifiez les vôtres dans Paramètres →
Appareils.

## 1. Thermostats

- `climate.ciat_heat_pump_heating` : chauffe / arrêt, presets **confort / éco / absent**, consigne 15-30 °C par 0,5.
- `climate.ciat_heat_pump_pool` : chauffe / arrêt (molette sur piscine / arrêt), consigne 20-35 °C par 0,5.

Leur état vient toujours de la PAC. Un changement prend quelques secondes : le navigateur doit parfois d'abord sortir
du menu des paramètres (~3 s), et la consigne est atteinte appui par appui (~0,5 s chacun), puis confirmée (~2 s).
Pendant le réglage, la carte montre la valeur monter pas à pas.

Un seul des deux peut chauffer à la fois : la piscine est une position de la même molette. Mettre la piscine en
chauffe met le thermostat de chauffage sur arrêt, et inversement.

## 2. Planning de la semaine

Le terminal d'origine gardait le programme hebdomadaire ; avec l'émulateur, **c'est Home Assistant qui doit fournir le
créneau**. N'importe quel planning qui règle le preset `comfort` / `eco` de `climate.ciat_heat_pump_heating`
convient. Le firmware ignore une demande qui ne change rien : le planning peut la répéter.

Conseillé : l'intégration **`ha-chauffage`** (HACS, par armellarcier). Contrairement à la programmation horaire
native de Home Assistant, sa carte permet à des **utilisateurs non administrateurs** (une tablette murale, par
exemple) de modifier la semaine, par tranches d'une demi-heure.

Réglages qui fonctionnent avec cet émulateur :

| Option | Valeur | Pourquoi |
|---|---|---|
| Utiliser les préréglages des thermostats | **activé** | sinon chaque créneau envoie une température, c'est-à-dire des appuis + et − sur la PAC |
| Préréglage Éco | `eco` du thermostat | |
| Préréglage Confort | `comfort` du thermostat | |
| Préréglage Hors-gel | **`away`** du thermostat | sinon il enverrait 7 ou 15 °C |
| Préréglage Boost | `comfort` du thermostat | la PAC n'a pas de boost |
| Niveau d'accès des non-administrateurs | Programmer | permet à l'utilisateur d'une tablette de modifier les semaines |
| Durée des dérogations | par exemple 120 min, ou jusqu'au créneau suivant | |

À savoir :

- Il applique un créneau à son début, au démarrage de Home Assistant et quand on enregistre les semaines ; il ne
  renvoie rien en boucle, donc les scans ne sont pas interrompus pour rien.
- **Tout changement fait ailleurs devient une dérogation** (preset, consigne ou mode), qui dure jusqu'à sa fin, puis
  le planning reprend. Mettre le thermostat sur arrêt est donc temporaire : pour couper durablement, mettez en pause
  l'interrupteur « Pilotage » de l'intégration.
- 🟡 À la fin d'une dérogation de consigne, il remet le preset, pas la température : la consigne modifiée reste dans
  la PAC (c'est un changement durable de sa consigne confort ou éco).
- ⚠️ **L'été** : si le planning reste actif, son créneau suivant remet la molette sur chaud et coupe le chauffage de
  la piscine. Mettez-le en pause pour l'été, ou liez la pause au thermostat piscine par une automatisation.

## 3. Notifications

[`automation-bus-notifications.yaml`](../examples/home-assistant/automation-bus-notifications.yaml) envoie une
notification :

- à chaque **anomalie du bus**, avec tout le diagnostic (trame, délai, écho attendu ou non, trame envoyée), même
  quand l'émission repart seule ;
- quand **l'émission reste coupée plus d'1 min** (reprises automatiques épuisées, ou coupure à la main).

Elle ne remet jamais l'émission en marche : c'est le firmware qui le fait, volontairement au plus 3 fois par heure.
Une automatisation qui la remettrait de force contournerait cette limite.

## 4. Tableau de bord

[`dashboard-view.yaml`](../examples/home-assistant/dashboard-view.yaml) est une vue « sections » avec :

- les deux thermostats ;
- les mesures P40-P48 et leurs courbes sur 24 h ;
- l'état de l'émulateur, la dernière anomalie et les résultats du navigateur ;
- les réglages lus dans la PAC ;
- les commandes bas niveau, affichées seulement quand le **Rescue mode** est en marche ;
- en option, la carte du planning `ha-chauffage`.

Sur une tablette murale, une seule tuile de thermostat (`target-temperature`, `climate-preset-modes`) et la carte du
planning dans une fenêtre surgissante suffisent pour l'usage quotidien.

## 5. Température ambiante

Le firmware envoie à la PAC le capteur Home Assistant choisi dans `room_sensor`. Prenez un thermomètre de la pièce
principale : la PAC s'en sert pour la compensation d'ambiance (P04). Elle est envoyée au dixième de degré (la
résolution du bus), et sa justesse compte : avec la compensation d'ambiance, chaque 0,1 °C d'erreur déplace la consigne
d'eau de 0,1 × P04 (0,35 °C avec P04 = 3,5 ; voir [protocole §8](protocol.fr.md#8-paramètres)). La dernière valeur valide est gardée en flash et
utilisée sans limite de durée si le capteur ou Home Assistant disparaît ; « Emulator state » affiche alors « room
temperature frozen ».

N'utilisez pas la « Terminal room temperature » de l'appareil lui-même : une fois le terminal d'origine débranché,
elle ne fait que relire ce qu'envoie l'ESP32.

## 6. Puissance calorifique et COP

[`package-heat-output.yaml`](../examples/home-assistant/package-heat-output.yaml) est un paquet Home Assistant qui
complète la **« Thermal power »** de l'émulateur (1,163 × débit × ΔT, voir [émulateur §4](emulator.fr.md#4-mesures-et-réglages)),
dont la justesse dépend du débit d'eau saisi. Il y ajoute une estimation indépendante, tirée des données du fabricant :

| Entité | Calcul |
|---|---|
| Heat pump COP datasheet | COP (puissance calorifique / puissance absorbée) du tableau CIAT, interpolé selon l'extérieur (P48) et le départ (P41) |
| Heat pump heat output datasheet | COP de la notice × (puissance électrique mesurée − circulateur) |
| Heat pump heat energy datasheet | son cumul en kWh |
| Heat pump estimated water flow | puissance notice / (1,163 × ΔT), compresseur en marche depuis plus de 3 min |
| Heat pump COP measured | puissance thermique de l'émulateur / puissance électrique |

Mode d'emploi :

1. Mesurez la puissance électrique de la PAC (prise connectée ou compteur) et indiquez son entité dans le paquet.
2. Laissez tourner quelques jours ; la moyenne du « estimated water flow » est le débit à saisir dans le « Water flow
   rate » de l'émulateur.
3. La puissance et l'énergie thermiques de l'émulateur deviennent alors la référence ; les valeurs de la notice
   restent pour comparer. Un COP mesuré qui décroche durablement du COP de la notice signale un encrassement, une
   perte de fluide ou des dégivrages ratés.

⚠️ La grille de COP du fichier est celle de l'**Aqualis 2 35H** (documentation CIAT N 08.01 A, puissances
calorifiques, dégivrages compris, circulateur exclu). Pour une autre taille, recalculez la grille à partir du même
tableau. La puissance du circulateur (155 W) a été mesurée sur l'installation de référence : adaptez-la.

Sur l'installation de référence (35H, 6 à 9 °C dehors), les deux estimations concordaient à ~10-20 % près avant le
recalage du débit ; le COP allait de ~3 (départ ~43 °C) à ~4,4 (départ ~31 °C).
