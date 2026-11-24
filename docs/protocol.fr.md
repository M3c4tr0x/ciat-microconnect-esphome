[English](protocol.md) · **Français**

# Protocole du bus CIAT Micro-Connect

Spécification du bus 2 fils entre le terminal d'ambiance **CIAT Micro-Connect** et la pompe à chaleur
**CIAT Aqualis 2** (carte de régulation « Micro-Connect »). Elle a été établie par rétro-ingénierie en novembre
2026 : 234 captures à l'oscilloscope, puis des semaines d'enregistrement continu par un ESP32 et, à partir du
23/11/2026, par un ESP32 qui remplace le terminal.

Repères utilisés partout :

- ✅ **confirmé**, par de nombreuses captures ou par l'émulateur en service sur la PAC ;
- 🟡 **hypothèse**, cohérente avec les données mais pas prouvée ;
- ❓ **inconnu**.

Installation de référence : version de la carte **P80 = 9.00**, version du terminal **P81 = 1.02**. D'autres
versions peuvent différer. Le même matériel serait vendu sous les marques Atlantic et De Dietrich (🟡).

## 1. Couche physique

| Point | Valeur | Statut |
|---|---|---|
| Câblage | 2 fils qui portent **à la fois l'alimentation et les données**, sur le connecteur J2, bornes 1-2 de la carte (« terminal de commande 12 V ») | ✅ |
| Polarité | le terminal a un pont de diodes (MB10S) en entrée : **la polarité est indifférente** | ✅ |
| Tension de repos | ~11,2 V avec le terminal, 11,6 V à vide | ✅ |
| PAC → terminal | la PAC tire le bus à **~0 V** pendant les bits à 0 | ✅ |
| Terminal → PAC | le terminal tire le bus à **~0,5 V** pendant ses bits à 0 (collecteur ouvert) | ✅ |
| Format | **UART 9600 bauds, 8 bits, sans parité, 1 bit de stop (8N1), LSB en premier** | ✅ |
| Consommation du terminal | ~1,5 mA en moyenne, ondulation de 1 à 5 mA (shunt de 22 Ω) | ✅ |

Comme les deux côtés émettent en tirant la même ligne vers 0, **un seul récepteur sur la tension du bus voit
les deux sens** ✅. Le terminal continue de fonctionner sur ses condensateurs pendant que la ligne est basse
(une diode sépare le bus de sa réserve d'alimentation).

> ⚠️ **Le bus n'est pas flottant.** Son retour est relié à la terre dans la PAC, à faible impédance en continu.
> Relier la masse d'un oscilloscope relié à la terre à un fil du bus **a arrêté la communication et mis la PAC
> en défaut** ; seule une coupure de l'alimentation du groupe extérieur l'a relancée. Toute interface doit être
> **isolée galvaniquement** (voir [matériel](hardware.fr.md)).

## 2. Trames et contrôle

- Pas de champ de longueur. Le type de trame est donné par sa place dans le cycle ; le premier octet est constant.
- ✅ **Le dernier octet est le XOR de tous les octets précédents** (17 604 trames décodées sur 17 604 lors de la
  relecture, les seules exceptions étant des trames coupées en bord de capture).
- Les requêtes de la PAC font 2 octets : la commande, puis son XOR, qui donne le même octet (`14 14`, `12 12`).

## 3. Cycle d'interrogation ✅

La PAC est maître. Elle répète ce cycle sans fin :

| # | Sens | Trame | Début → début de la trame suivante |
|---|---|---|---|
| Q14 | PAC → terminal | `14 14` | 10,2 à 21,5 ms (médiane 10,75) |
| 14.1 | terminal → PAC | `EE jj HH MM xx` (5 octets) | 24,7 à 48,7 ms |
| Q12 | PAC → terminal | `12 12` | 10,6 à 22,1 ms (médiane 11,1) |
| 12.1 | terminal → PAC | `AC b1 b2 TT Fd 12 xx` (7 octets) | 13,6 à 18,5 ms |
| 12.2 | PAC → terminal | `40 p1 p2 d1 d2 d3 d4 xx` (8 octets) | 140 à 164 ms |

- Un cycle dure 207 à 245 ms, médiane **~222 ms** (~270 cycles par minute). Un octet dure 1,04 ms.
- Le terminal répond **10,2 à 11,2 ms** après le début de la requête ; ~1,5 % de ses réponses arrivent entre
  12 et 22 ms. ✅ **La PAC accepte une réponse au moins jusqu'à ~22 ms** après le début de la requête (mesure
  continue avec horodatage par front). Au-delà : ❓.
- **Sans réponse** (terminal débranché) : la PAC passe à la requête suivante au bout de 43 à 55 ms, n'envoie
  pas de 12.2, et continue d'interroger (`14 14`, `12 12`) toutes les ~106 ms ✅.
- **Il n'existe aucune autre trame** ✅ : vérifié sur toutes les captures, y compris le démarrage, tout le menu
  des paramètres, l'édition du programme et tous les modes.

## 4. Carte des registres

Bits de b7 (à gauche) à b0.

**14.1 — terminal → PAC : horloge** `EE jj HH MM xx`

| Octet | Contenu | Statut |
|---|---|---|
| 0 | `EE`, constant | ❓ sens |
| 1 | jour de la semaine, **1 = lundi … 7 = dimanche** | ✅ |
| 2 | heure, **BCD** (`22` = 22 h) | ✅ |
| 3 | minutes, **BCD** | ✅ |
| 4 | XOR des octets 0 à 3 | ✅ |

L'horloge appartient au terminal (sauvegardée par un supercondensateur). La PAC ne la règle jamais.

**12.1 — terminal → PAC : état du terminal** `AC b1 b2 TT Fd 12 xx`

| Octet | b7 | b6-b5 | b4 | b3 | b2-b0 |
|---|---|---|---|---|---|
| 1 `b1` mode | 0 | **créneau du programme** ✅ : `01` confort, `10` éco, `11` pas de demande | 1 | 1 | **recopie des bits 2-0 de p1 du cycle précédent** ✅ |

| Octet | Contenu | Statut |
|---|---|---|
| 0 | `AC`, constant | ❓ sens |
| 2 `b2` | bits 6-3 : **position de la molette** (1 heure, 2 prog froid, 3 froid, 4 arrêt, 5 hors-gel, 6 chaud, 7 prog chaud, 8 piscine) ; bit 2 **OK**, bit 1 **−**, bit 0 **+** (1 = appuyé) | ✅ |
| 3 `TT` | température ambiante, partie entière, **BCD** (`23` = 23 °C) | ✅ de 13,1 à 31,0 °C ; sous 10 °C non vérifié |
| 4 `Fd` | quartet haut toujours `F` (🟡 signe ou validité ?) ; quartet bas = **dixièmes** de l'ambiante | ✅ dixièmes ; ❓ `F` |
| 5 | `12`, constant ; 🟡 version du terminal en BCD (P81 = 1.02) | 🟡 |
| 6 | XOR des octets 0 à 5 | ✅ |

Valeurs courantes de `b1` ✅ : `7E` première trame après la mise sous tension (une seule fois), `3D` programmé,
créneau confort, `5D` programmé, créneau éco, `78` manuel, arrêt, hors-gel, piscine, froid, `79` heure,
`7A` édition du programme froid, `7B` édition du programme chaud.

- Le champ du créneau (bits 6-5) ne diffère de `11` que lorsque le code de mode vaut 5 (programmé). Là, **le
  terminal indique à la PAC le créneau en cours de son programme hebdomadaire** ; la PAC répond avec le bit
  confort ou éco correspondant de p1, dans le même cycle ✅.
- **Les bits 2-0 de b1 valent les bits 2-0 de p1 du cycle précédent** ✅ (3 306 cycles consécutifs, aucune
  exception). La PAC calcule le code de mode à partir de la molette et de son propre état ; le terminal le
  recopie.
- Exemple de `b2` : molette sur chaud, aucune touche = `0 110 000` = `30` ; chaud + OK = `34`.
- Le terminal envoie l'état **brut** des touches et de la molette. **C'est la PAC qui interprète les appuis**
  (consigne, menu, appuis longs) ✅.

**12.2 — PAC → terminal : affichage** `40 p1 p2 d1 d2 d3 d4 xx`

| Octet | b7 | b6 | b5 | b4 | b3 | b2-b0 |
|---|---|---|---|---|---|---|
| 1 `p1` | sablier, clignotant ✅ | **éco** ✅ | **confort** ✅ | ❓ | **rétroéclairage** ✅ | **code de mode** ✅ (recopié dans b1) |
| 2 `p2` | ❓ | **compresseur en marche** ✅ | ❓ | ❓ | ❓ | bit 2 **piscine, clignotant** ✅ ; bit 1 **menu des paramètres** ✅ ; bit 0 **niveau installateur** ✅ |

| Octet | Contenu | Statut |
|---|---|---|
| 0 | `40`, constant | ❓ sens |
| 3-6 | les **4 chiffres**, en code 7 segments (§5) | ✅ |
| 7 | XOR des octets 0 à 6 | ✅ |

- **Code de mode** (bits 2-0 de p1) : 0 manuel ou autre, 1 heure, 2 prog froid, 3 prog chaud, 5 programmé,
  6 démarrage. ✅ sens de la recopie ; 🟡 sens exact de chaque code.
- **Rétroéclairage** (0x08) : mis par la PAC à l'appui d'une touche ou au mouvement de la molette, retombe
  ~4 s plus tard ✅.
- **Sablier** (0x80) : clignote en mode essai (P30 = 1), comme le dit la notice ✅.
- **Compresseur** (p2 0x40) : suit le compresseur réel à une ou deux secondes près ✅.
- **Piscine** (p2 0x04) : clignote (~0,9 s allumé, 0,9 s éteint) pendant l'attente du contact piscine ✅.
- **Menu** (p2) : `02` niveau client, `03` niveau installateur ✅.
- Les bits 0x08, 0x10, 0x20 et 0x80 de p2 n'ont jamais été vus. La notice cite des pictogrammes pour l'appoint
  électrique, la chaudière, le mode essai, le délestage et le dégivrage ❓. **Un dégivrage ne change pas p2** ✅
  (un dégivrage observé).

## 5. Code 7 segments ✅

bit 0 = segment a (haut), bit 1 = b, bit 2 = c, bit 3 = d, bit 4 = e, bit 5 = f, bit 6 = g (milieu),
**bit 7 = point décimal** (vu seulement sur d3 : un point fixe entre le 2ᵉ et le 3ᵉ chiffre).

| Code | Car. | Code | Car. | Code | Car. |
|---|---|---|---|---|---|
| `3F` | 0 | `6D` | 5 | `40` | - |
| `06` | 1 | `7D` | 6 | `58` | c (°C) |
| `5B` | 2 | `07` | 7 | `73` | P |
| `4F` | 3 | `7F` | 8 | `00` | vide |
| `66` | 4 | `6F` | 9 | `5E` | d (🟡 codes défaut, jamais vus) |

Exemples : `06 7F ED 58` = « 18.5c » ; `00 73 4F 06` = « P31 » ; `40 40 40 40` = « ---- » ; `00 06 BF 5B` = « 1.02 ».

## 6. Répartition des rôles ✅

| Terminal | PAC |
|---|---|
| horloge (jour, heure, minute), avec sauvegarde | consignes confort, éco, hors-gel et piscine |
| programmes hebdomadaires chaud et froid, et leur affichage | paramètres P00 à P99, dont la **loi d'eau** (P10 à P19) |
| créneau en cours du programme, confort ou éco (b1) | consigne d'eau (P42) d'après la température extérieure et la compensation d'ambiance P04 |
| mesure de la température ambiante | régulation du compresseur sur la **température de retour d'eau**, dégivrage, défauts |
| position de la molette, état brut des touches | menu des paramètres, durée des appuis, **tout ce qu'affichent les 4 chiffres** |

**Le programme hebdomadaire n'existe que dans le terminal** et n'est jamais transmis ✅. Un terminal de
remplacement doit fournir lui-même le créneau en cours (ici : Home Assistant).

## 7. Comportements

### 7.1 Démarrage et connexion

- Après la mise sous tension, le terminal se tait ~1 s, puis envoie une 12.1 avec **b1 = `7E`** ✅.
- La PAC répond à `7E` par un **test des segments** : `40 F8 FF FF FF FF 7F C7` (« `.8.8.88` », tous les
  pictogrammes allumés), puis des trames normales ✅.
- Tant qu'elle n'a pas reçu de créneau, la PAC affiche l'**éco** en programmé ; elle passe en confort dès que le
  terminal envoie `3D` ✅.
- **Sans terminal, la PAC continue de fonctionner** avec son dernier état : circulateur en marche, compresseur
  qui démarre et s'arrête seul ✅. Mais **après une coupure de courant, rien ne démarre tant qu'aucun terminal
  ne répond** : 0 W jusqu'au premier échange, qui suffit à lancer le circulateur ✅.

### 7.2 Molette

- Quand la molette change, **p1 suit après ~5 cycles (~1,1 s)**, et b1 recopie p1 un cycle plus tard ✅.
- Le terminal échantillonne la molette lentement : les positions traversées rapidement ne sont jamais
  envoyées ✅.
- Affichages : hors-gel `10.0c`, arrêt `----`, piscine = consigne piscine (P23), avec le bit piscine
  clignotant ✅.

### 7.3 Touches hors menu (consigne)

- **+ et − changent la consigne du mode en cours par pas de 0,5 °C** : en programmé, la consigne confort ou éco
  elle-même, de façon durable ✅. Plage en chaud **15,0 à 30,0 °C** (saturation aux bornes) ✅ ; la consigne
  piscine accepte au moins 20 à 35 °C ✅ (bornes non explorées au-delà).
- ✅ **Hors menu, une touche doit être tenue au moins 3 cycles** (~460 ms) : la PAC réagit pendant la 3ᵉ trame.
  Sur 2 trames, aucune réaction (10 appuis sur 10). Tenue plus longtemps, la valeur se répète.
- **OK** fait passer de programmé à confort forcé puis à éco forcé (b1 devient `78` en mode forcé). Il agit
  même écran éteint ✅.
- + et − ne font pas quitter le programmé ✅.

### 7.4 Menu des paramètres

- **Entrée** : + et OK tenus ensemble. Le **niveau client** s'ouvre au bout de ~2,95 s (p2 `02`) ; le **niveau
  installateur** ~5,1 s plus tard, soit ~8 s depuis le début de l'appui (p2 `03`) ✅. Seul le niveau
  installateur montre tous les paramètres : P00, P06-P09, P20-P21, P24-P30, P43, P46, P47 et P49, entre
  autres, sont masqués au niveau client ✅.
- Dans le menu, **une trame suffit** pour une touche, et la PAC réagit **dans le même cycle** (10-20 ms) ✅.
  Tenues à partir de la 4ᵉ trame, les touches se répètent (4, 3, 3, 2, 2 cycles, puis un pas par cycle) ✅.
- **Navigation** : + / − changent le numéro du paramètre ; les numéros absents ou masqués sont sautés ; **la
  liste boucle** (P00 ↔ P99 au niveau installateur, P05 ↔ P81 au niveau client) ✅.
- **Reprise** : le menu se rouvre sur le paramètre **qui suit le dernier consulté** (en tenant compte du
  niveau) ✅. Une coupure de courant de la PAC la remet à P05 : la position est gardée en mémoire vive ✅.
- **OK** sur un numéro affiche sa valeur ; + / − la changent (pas de 1 ou de 0,1, **saturation** aux bornes,
  pas de bouclage) ✅ ; **OK revient au numéro et valide la valeur** ✅.
- **Sortie** : OK tenu ; le menu se ferme ~12-13 cycles (~2,7-2,9 s) après le début de l'appui ✅. Si une
  valeur est affichée, l'OK tenu revient d'abord au numéro, **ce qui valide la valeur** ✅.
- La PAC **continue de réguler pendant le menu** (démarrages et arrêts du compresseur, un dégivrage complet vu
  pendant 5 h de séances de menu) ✅. Un changement de créneau qui arrive pendant le menu est appliqué à la
  sortie ✅ (un cas).
- Les mesures sont affichées **brutes** : P50 change presque à chaque cycle ; il faut moyenner plusieurs
  cycles ✅.

### 7.5 Édition du programme (terminal seul)

Le programme hebdomadaire s'édite dans le terminal (molette sur une position de programme). Le bus ne montre
que les touches brutes et b1 `7A` / `7B` ; **la PAC ne réagit à aucun de ces appuis** ✅. L'édition s'ouvre
toujours sur lundi ; chaque OK bref passe au jour suivant ; un OK long passe au jour suivant et y copie le jour
quitté (pas de copie de dimanche sur lundi) ✅.

### 7.6 Observations inexpliquées ❓

- Quelques **trames 12.2 d'un seul cycle** avec une consigne ou une valeur +0,1 (par exemple 22.1c entre deux
  22.0c), XOR juste.
- **Trames `AE`** du terminal d'origine (`AC` avec le bit 0x02 en plus dans l'en-tête et dans le XOR), à
  intervalles irréguliers (de quelques minutes à quelques heures, ~1-2 h en moyenne), ignorées par la PAC.
  **Aucune** depuis qu'un ESP32 remplace le terminal : 🟡 une particularité du terminal d'origine, pas un
  parasite du bus.

## 8. Paramètres

Source : la fiche de mise en service CIAT Habitat « Liste des Paramètres Aqualis 2 » (2 pages, non redistribuée
ici), lue sur l'original. **UT** = radiateurs / ventilo-convecteurs ou mixte avec plancher (P03 = 0) ; **PCR** =
plancher chauffant rafraîchissant (P03 = 1). 👤 = affiché au niveau client d'après la fiche (sur l'installation de
référence, le bus n'est pas d'accord pour P00, P05, P43 et P60, voir plus bas). « Lecture » = valeur en direct, non
modifiable.

| P | Signification | Valeurs / plage | Usine |
|---|---|---|---|
| P00 👤 | version machine | 1 = Aqualis, 2 = Aqualis 2 | 2 |
| P01 | présence de glycol | 0 = eau pure, 1 = eau glycolée | 0 |
| P02 | entrée TOR sur J2 bornes 3-5 | 0 = non utilisée, 1 = forçage des modes chaud et froid, 2 = mise en hors-gel à distance | 0 |
| P03 | émetteurs de chauffage | 0 = radiateurs / ventilo-convecteurs ou mixte avec plancher (UT), 1 = plancher chauffant rafraîchissant (PCR) | 1 |
| P04 | coefficient de compensation d'ambiance | 0 à 5 | 2 |
| P05 | fonctionnement bas niveau sonore | 0 = standard, 1 = bas niveau sonore en froid, 2 = en chaud, 3 = en froid et en chaud | 2 |
| P06 | appoints et possibilité de délestage | 0 = PAC seule (délestage PAC possible) ; 1 = PAC + appoints électriques (délestage des appoints) ; 2 = PAC + appoints électriques (délestage PAC et appoints) ; 3 = PAC **ou** appoint chaudière (délestage PAC) ; 4 = PAC + appoint chaudière (délestage PAC et chaudière) ; 5 = PAC + appoint chaudière (délestage PAC) | 0 |
| P07 | T° extérieure d'autorisation des appoints | de P09 à 24 °C (affiché seulement avec appoints électriques, P06 = 1 ou 2) | 5 °C |
| P08 | validation compresseur | 0 = compresseur non autorisé à fonctionner, 1 = autorisé | 1 |
| P09 | limite basse de fonctionnement en chaud (air) | −15 à 24 °C | −15 °C |
| P10 👤 | consigne confort eau en froid | UT : 10 à 28 °C (P01 = 0), −6 à 28 °C (P01 = 1) ; PCR : 20 à 25 °C (P22 = 0 ou 1), 18 à 25 °C (P22 = 2 ou 3) | UT 12 °C / PCR 23 °C |
| P11 👤 | consigne économique eau en froid | comme P10 | UT 12 °C / PCR 23 °C |
| P12 👤 | T° air extérieur début de dérive en froid | UT : 10 à 40 °C ; PCR : sans objet | UT 25 °C |
| P13 👤 | T° air extérieur fin de dérive en froid | UT : P12 + 5 à 45 °C ; PCR : sans objet | UT 35 °C |
| P14 👤 | consigne max. en fin de dérive en froid | UT : 10 à 28 °C (P01 = 0), −6 à 28 °C (P01 = 1) ; PCR : sans objet | UT 15 °C |
| P15 👤 | consigne confort eau en chaud | 20 °C à P19 | UT 30 °C / PCR 20 °C |
| P16 👤 | consigne économique eau en chaud | 20 °C à P19 | UT 30 °C / PCR 20 °C |
| P17 👤 | T° air extérieur début de dérive en chaud | UT : −10 à 30 °C ; PCR : 17 à 25 °C | 20 °C |
| P18 👤 | T° air extérieur fin de dérive en chaud | UT : −15 °C à P17 − 5 ; PCR : −15 à 8 °C | −7 °C |
| P19 👤 | consigne max. en fin de dérive en chaud | UT : 20 à 49 °C (P06 = 0), 20 à 55 °C (P06 = 1 ou 2), 20 à 70 °C (P06 = 3, 4 ou 5) ; PCR : 20 à 40 °C | UT 40 °C / PCR 35 °C |
| P20 | différentiel d'étage | 0,5 à 5 °C | 2 °C |
| P21 | différentiel entre étages (affiché seulement avec appoints) | 0,5 à 10 °C | 2 °C |
| P22 👤 | options piscine et sonde hygrométrique | 0 = aucune, 1 = gestion piscine, 2 = gestion sonde hygrométrique, 3 = piscine + sonde hygrométrique | 0 |
| P23 👤 | consigne eau piscine | 20 à 35 °C | 28 °C |
| P24 | temps de givrage auto-adaptatif | 0 = non, 1 = oui | 0 |
| P25 | T° batterie de déclenchement de la tempo avant dégivrage | −5 à 0 °C | −2 °C |
| P26 | retour d'eau min. d'autorisation du dégivrage | UT : 25 °C (P01 = 0), 10 °C (P01 = 1) ; PCR : 20 °C (P01 = 0), 5 °C (P01 = 1) | 20 °C |
| P27 | T° batterie de fin de dégivrage | 10 à 45 °C | 35 °C |
| P28 | temps de givrage | 10 à 90 min | 30 min |
| P29 | arrêt compresseur en fin de dégivrage | 0 = pas d'arrêt, 1 = arrêt de 3 min (la notice ajoute 2 = arrêt d'1 min, usine 2) | 0 |
| P30 | mode essai (temporisations de sécurité raccourcies, annulé au bout d'1 h, sablier clignotant) | 0 = non, 1 = oui | 0 |
| P31 👤 | consigne du mode hors-gel (air) | 8 à 15 °C | 10 °C |
| P32 | variation max. de la T° fréon échangeur (protection antigel) | 0 à 5 °C | 1 °C |
| P33 | variation max. de la T° de sortie d'eau (protection antigel) | 0 à 1 °C | 0,3 °C |
| P34 | asservissement de la marche du circulateur (affiché avec une chaudière) | 0 = arrêt si fonctionnement chaudière, 1 = marche en continu | 1 |
| P36 | autorisation du mode froid | 0 = mode froid interdit, 1 = autorisé | 1 |
| P40 👤 | **lecture** température retour d'eau | | |
| P41 👤 | **lecture** température départ d'eau | | |
| P42 👤 | **lecture** consigne d'eau calculée (`----` quand elle n'est pas calculée, par ex. en hors-gel) | | |
| P43 👤 | **lecture** différentiel d'étage corrigé | | |
| P44 👤 | **lecture** T° fréon batterie extérieure | | |
| P45 👤 | **lecture** T° fréon échangeur | | |
| P46 | **lecture** temporisation anti-court-cycle : compte à rebours affiché en **minutes.secondes** (`2.52` = 2 min 52 s), depuis 3:00 à l'arrêt du compresseur ✅ ; valeurs au-dessus de 3:00 juste après un démarrage (4:31, 29 s après l'un d'eux) ❓ | | |
| P47 | **lecture** temps de givrage restant (min) | | |
| P48 👤 | **lecture** T° extérieure | | |
| P49 | **lecture** compteur du nombre de resets défaut | | |
| P50 👤 | **lecture** T° de la piscine | | |
| P60 | option production ECS | 0 = sortie chaud / froid, 1 = chauffage du ballon d'ECS 1 cycle par jour, 2 = 2 cycles par jour | 0 |
| P61 | heure de début ECS cycle 1 (masqué si P60 = 0) | 00 h 00 à 23 h 30 | 1 h 30 |
| P62 | heure de début ECS cycle 2 | P61 + 6 h à 23 h 30 | 12 h 30 |
| P63 | durée max. en ECS de la PAC quand le mode chaud est sélectionné | 0 h 30 à 2 h 00 | 2 h |
| P80 👤 | **lecture** n° de version de la carte de régulation | | |
| P81 👤 | **lecture** n° de version du terminal | | |
| P93 | limite antigel évaporateur sur départ d'eau | fixe à 3 °C en eau pure (P01 = 0) ; −10 à +3 °C en eau glycolée (P01 = 1) | +3 °C (P01 = 0) / 0 °C (P01 = 1) |
| P94 | limite antigel échangeur sur fréon | non réglable | P93 − 5 °C |
| P95 | limite haute du retour d'eau échangeur en mode froid | non réglable | 40 °C |
| P96 | limite haute du retour d'eau échangeur en mode chaud | non réglable | 10 °C (P01 = 0) / 5 °C (P01 = 1) |
| P98 | fluide utilisé | 1 = R410A | 1 |
| P99 | version froid seul / réversible | 0 = froid seul, 1 = réversible | selon le modèle |

Numéros qui n'existent pas : P35, P37-P39, P51-P59, P64-P79, P82-P92, P97 ✅. Paramètres masqués par la configuration
(✅ sur le bus) : P07 et P21 sans appoint électrique, P34 sans chaudière, P61-P63 quand P60 = 0.

Bus et fiche pour le niveau client, sur l'installation de référence (carte P80 = 9.00) : P05 et P60 sont affichés au
niveau client alors que la fiche ne leur donne pas d'icône ; P00 et P43 ne le sont pas, alors qu'elle leur en donne
une. Pour cette carte, le bus fait foi.

### 8.1 Cartes de régulation plus récentes (P80 = 22.00)

Une fiche CIAT plus récente, « Paramétrage et liste code défauts — µconnect 2 et µconnect 2+ ou 2x, version P80 =
V22.00 » (plan 3989622, indice 06, 11/2018 ; [copie hébergée par un installateur](https://www.lemble-pac.fr/wp-content/uploads/2025/02/aqualis-2-parametre-et-defaut.pdf)), liste **plus de
paramètres**. Aucun n'existe sur la carte de référence (P80 = 9.00) : le menu saute P51-P59, P64-P79 et P82-P92 ✅.
Ils dépendent de la version de la carte.

| P | Signification (V22.00) | Valeurs / usine |
|---|---|---|
| P53 | compteur des défauts d1.4 et d1.4P | lecture, 0 à 9999 |
| P54 | nombre de défauts d1.2 (temporaires ou permanents) | lecture |
| P55 / P56 | démarrages du compresseur / démarrages ÷ 10 000 | lecture |
| P57 / P58 | heures de fonctionnement du compresseur / heures ÷ 10 000 | lecture |
| P59 | cycles de dégivrage thermodynamiques | lecture |
| P70 / P71 | tension min. / max. du ventilateur en dégivrage (sortie 0-10 V, ventilateur EC) | selon le modèle (P82) |
| P73, P74, P76, P77 | tensions du ventilateur : bas niveau sonore / ECS / piscine, max. et min. en chaud-froid, démarrage en froid | selon le modèle |
| P82 | référence de la PAC | 20H, 28H, 35t, 35H, 35Ht, 50t, 50H, 50Ht, 65t, 65Ht, 75t, 75Ht |
| P83 | temps d'arrêt minimum du compresseur (arrêt par la régulation) | 6 à 15 min (monophasé, usine 6) ou 3 à 15 min (triphasé, usine 3) |
| P84 | type de ventilateur | AC (triac) ou EC (0-10 V) |

Valeurs qui **diffèrent de l'ancienne fiche** en V22.00 (🟡 non vérifié sur une carte de cette version) :

- P02 : 0 = terminal local, ou ECS si P60 = 3 ; 1 = entrées TOR chaud / froid ; 2 = terminal avec gestion d'absence
  par mise en hors-gel.
- P03 : ajoute 2 = UTR + PCR et 3 = 2 × PCR.
- P19 : usine 49 °C (UTR) avec P00 = 2.
- P25 / P27 : usine −2 °C / 35 °C en R410A (P98 = 1), 0 °C / 40 °C en R407C (P98 = 0).
- P29 : temps d'arrêt minimum du compresseur pendant les cycles de dégivrage, 1 = 3 min (démarreur monophasé),
  2 = 1 min ; usine 1.
- P30 : 0 = non, 1 = mode essai avec arrêt minimum d'1 min (triphasé), 2 = avec arrêt de 3 min (monophasé).
- P34 : ajoute 2 = mode économique avec arrêt si la chaudière fonctionne, 3 = mode économique avec arrêt du
  circulateur si P48 > P17 + 2.
- P60 : ajoute 3 = ECS pilotée par un contact externe.
- P98 : 0 = R407C, 1 = R410A.

### 8.2 Codes défaut

D'après la même fiche V22.00 ✅ (document) ; **leur aspect sur le bus n'a jamais été observé** ❓. Ils s'affichent sur
les 4 chiffres (`d` = `5E` en code 7 segments, puis des chiffres, par exemple « d1.4 », « d4.6 ») ; le décodeur
accepte ces codes.

- Défaut **temporaire** : fonctionnement dégradé, affichage **clignotant**, réarmement automatique au retour des
  conditions normales.
- Défaut **permanent** : arrêt du groupe (appoints autorisés si configurés), affichage **fixe**. Réarmement :
  **molette sur arrêt, OK tenu 3 s**.
- En mode ECS, le défaut s'affiche suivi d'un `S` (affiché comme un 5), par exemple « d3. 5 », jusqu'au réarmement par
  OK en position arrêt ou un nouveau cycle d'ECS.

| Code | Composant | Cause | Niveau | Conséquence | Réarmement |
|---|---|---|---|---|---|
| (aucun) | liaison avec le terminal | aucune trame du terminal | temporaire | fonctionne sur le dernier mode demandé | automatique au retour du dialogue ✅ (observé) |
| d1.1 | compresseur / débit | rotation inverse du compresseur triphasé ; mode essai actif (monophasé, P30 = 1) ; débit d'eau trop important (P41 − P40) | temporaire, permanent après 5 en 24 h | PAC à l'arrêt (appoints autorisés) | 5 min / manuel |
| d1.2 | circuit frigorifique | haute pression en marche | temporaire, permanent après 5 en 24 h | PAC à l'arrêt (appoints autorisés) | 6 min / manuel |
| d1.4 | démarreur monophasé | tension < 202 V (démarreur CIAT) ou < 190 V (RSBS2332A) ; contact J3 (3-4) fermé | temporaire | PAC à l'arrêt ; redémarrage retardé de 6 min, 30 min, puis 1 h sur 24 h | automatique |
| d1.4P | démarreur RSBS2332A | rampe incomplète, surintensité > 80 A / 1 s | permanent | PAC à l'arrêt | mise hors tension |
| d1.5 | thermostat limiteur de plancher | eau d'entrée du plancher > 60 °C | permanent | PAC à l'arrêt, appoints interdits | réarmement du thermostat, puis manuel |
| d2 | vanne 4 voies | vanne bloquée ; mode essai actif (monophasé) | temporaire / permanent | PAC à l'arrêt (appoints autorisés) | 5 min / manuel |
| d3 | circulateur | débit absent ou faible, air, panne de pompe (pressostat différentiel) | temporaire / permanent | PAC à l'arrêt (appoint chaudière si P34 = 0) | temporisation / manuel |
| d4.1 | sonde de température extérieure | ouverte ou en court-circuit | temporaire en froid, permanent en chaud | PAC à l'arrêt | automatique en froid / manuel en chaud |
| d4.2 | sonde d'entrée d'eau | ouverte ou en court-circuit | permanent | PAC à l'arrêt | manuel |
| d4.3 | sonde de sortie d'eau | ouverte ou en court-circuit | permanent | PAC à l'arrêt (appoints autorisés) | manuel |
| d4.4 | sonde fréon batterie | ouverte ou en court-circuit | permanent | PAC à l'arrêt (appoints autorisés) | manuel |
| d4.5 | sonde fréon échangeur | ouverte ou en court-circuit | permanent | PAC à l'arrêt | manuel |
| d4.6 | **sonde d'ambiance du terminal** | ouverte ou en court-circuit | temporaire | fonctionne sur la consigne d'eau, sans compensation d'ambiance | automatique |
| d4.7 | sonde de température piscine | ouverte ou en court-circuit | temporaire | PAC à l'arrêt | automatique |
| d5 | limites de fonctionnement | eau ≥ P95 (froid) ou ≤ P96 (chaud), extérieur ≥ 43 °C (R407C) ou < P09 (chaud) ; retour d'eau < P26 empêchant le dégivrage (permanent) | temporaire / permanent | PAC à l'arrêt (appoints autorisés) | automatique / manuel |
| d6 | antigel de l'échangeur | sonde de sortie d'eau | temporaire / permanent | PAC à l'arrêt (appoints autorisés) | jusqu'à P41 > P93 + 6 K / manuel |
| d7 | antigel de l'échangeur | sondes sortie d'eau et fréon, en froid seulement | temporaire / permanent | PAC à l'arrêt | 15 min / manuel |
| d8 | antigel de l'échangeur | sortie d'eau et fréon, en froid et pendant un dégivrage | permanent | PAC à l'arrêt | manuel |
| d9.1 | EEPROM du terminal | dialogue avec le terminal | temporaire | derniers réglages enregistrés, sans programme hebdomadaire | automatique ou mise hors tension |
| d9.2 | EEPROM de la régulation, ou nouvelle version de logiciel | autodiagnostic | temporaire | derniers réglages enregistrés, sans programme hebdomadaire | automatique ou mise hors tension |

Les sondes sont des CTN de 10 kΩ à 25 °C. d4.6 concerne la sonde d'ambiance **du terminal** : 🟡 on ne sait pas
comment la PAC la détecte (valeur hors plage, ou quartet `F` de Fd, §4) ; un émulateur doit toujours envoyer une
ambiante plausible.

## 9. Pas encore observé

Tout ce qui précède vient d'une seule installation, qui **n'est jamais passée par certains modes**. Leurs trames sont
inconnues ou seulement supposées ; un terminal de remplacement doit être vérifié avant de s'y fier.

| Mode ou affichage | Pourquoi il n'a pas été vu | Ce qu'on sait |
|---|---|---|
| **Froid** et programme froid (molette en positions 2 et 3) | froid interdit sur cette installation (P36 = 0) | affichage `----` ; consignes froid, pictogrammes et créneau du programme froid ❓ |
| **Appoint électrique / chaudière** en marche | appoints désactivés pendant la campagne (P06 = 0), puis chaudière (P06 = 3) jamais déclenchée pendant les relevés | pictogrammes probables sur p2 0x08 / 0x10 / 0x20 / 0x80 ❓ |
| **Eau chaude sanitaire** | pas d'option ECS (P60 = 0) : P61-P63 masqués | ❓ |
| **Délestage** (pictogramme `€`) | pas de contact de délestage câblé (🟡) | ❓ |
| **Codes défaut** `dX.Y` | aucun défaut pendant les relevés | liste et sens connus par CIAT (§8.2) ; trames sur le bus ❓ (clignotement d'un défaut temporaire : sans doute un affichage alterné) |
| Pictogramme **dégivrage** | un dégivrage observé : **rien n'a changé sur le bus** | 🟡 la PAC ne signale pas le dégivrage au terminal |
| Température ambiante **sous 10 °C** ou négative | jamais atteinte | le quartet `F` de Fd ❓ ; l'émulateur plafonne à 10,0 °C |
| Entrée hors-gel à distance / forçage chaud-froid (P02) | entrée non utilisée | ❓ |
| Option hygrostat (P22 = 2) | option piscine utilisée à la place | ❓ |
| Mode essai (P30 = 1) | vu ~16 s seulement | sablier clignotant (p1 0x80) ✅ ; effets sur la régulation ❓ |

Si vous avez une installation dans l'un de ces cas, des captures du bus (émulateur en écoute seule, capteurs « New
heat-pump frame » et « Last rejected bytes ») combleraient ces trous.

## 10. Questions ouvertes

| # | Question | Statut |
|---|---|---|
| O2 | pictogrammes de p2 jamais vus (appoint, chaudière, délestage, défaut) | ❓ surveillance longue durée |
| O7 | aspect d'un code défaut `dX.Y` sur le bus | ❓ codes connus (§8.2), trames jamais vues |
| O10 | sens des octets constants `EE`, `AC`, `12`, `40` | ❓ 🟡 `12` = version du terminal |
| O11 | cause exacte du défaut de la PAC provoqué par une sonde reliée à la terre | ❓ |
| O12 | le quartet `F` de Fd ; températures ambiantes négatives | 🟡 |
| — | tolérance de la PAC aux réponses tardives au-delà de ~22 ms | ❓ |
| — | ce qu'applique la PAC après une coupure de courant suivie d'une longue période sans terminal | ❓ |
