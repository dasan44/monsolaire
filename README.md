# ALMADA – Afficheur d'énergie solaire sur ESP32 (écran rond)

Tableau de bord pour l'écran rond **ESP32-2424S012C** : il affiche en temps réel la production solaire et la consommation de la maison à partir d'un **Shelly EM**, avec un bilan quotidien, la météo, l'heure et un Pokémon aléatoire. Les écrans défilent automatiquement ou au toucher.

## Fonctionnalités

| Écran | Contenu | Durée |
|---|---|---|
| **Énergie** | Production solaire, consommation de la maison et échange avec EDF (Linky) en W ou kW, avec trois jauges en arc. Les couleurs changent selon le sens du Linky (rouge = je soutire, vert = j'injecte, jaune = équilibre), avec une zone neutre de ±10 W. | 15 s |
| **Cumul du jour** | Bilan en kWh depuis minuit : deux colonnes (production / consommation) à la même échelle, avec l'autoconsommation et l'autonomie en %. Remise à zéro automatique chaque jour à minuit. | 10 s |
| **Météo** | Température, humidité, ressenti, vent, min/max, probabilité de pluie, lever/coucher du soleil, icône dessinée selon le temps et fond coloré selon jour/nuit (données Open-Meteo). | 10 s |
| **Horloge** | Jour, date complète et heure (synchronisée par Internet, heure d'été/hiver automatique). | 10 s |
| **Pokémon** | Un Pokémon aléatoire (image, nom en français, types), différent à chaque passage. | 10 s |

## Rendu
<img width="1235" height="1306" alt="image" src="https://github.com/user-attachments/assets/9f4711bc-7b94-4f0d-ae1d-9f6a69f984fe" />
<img width="708" height="739" alt="image" src="https://github.com/user-attachments/assets/a00cb5c9-0ac4-499a-b9a4-49e915308f00" />
<img width="868" height="877" alt="image" src="https://github.com/user-attachments/assets/f75957c3-5d7d-4f12-b37d-2749c6a4191a" />
<img width="1345" height="1417" alt="image" src="https://github.com/user-attachments/assets/bd6f571a-ef94-4602-b7cf-55912fa3f3dd" />
<img width="1331" height="1334" alt="image" src="https://github.com/user-attachments/assets/6caf6836-a06e-42cf-bf1e-e80f0906bfa6" />
<img width="1338" height="1349" alt="image" src="https://github.com/user-attachments/assets/5082d6e1-3791-4a49-bdc7-29ab029d7147" />
<img width="1336" height="1355" alt="image" src="https://github.com/user-attachments/assets/44c82ee8-3d07-434e-80c5-50ce7889d209" />


### Autres éléments :
- **Animation de démarrage** de style interface futuriste, affichée uniquement à l'allumage, pendant la connexion au WiFi.
- **Transitions** entre les écrans (rideaux + fondu du rétroéclairage).
- **Tactile** : un appui passe à l'écran suivant.

## Matériel

- Carte **ESP32-2424S012C** : ESP32-C3, écran rond **GC9A01** 240×240, tactile **CST816**.
- **Shelly EM (Gen1)** avec deux pinces ampèremétriques :
  - une pince au niveau du compteur Linky (réseau) ;
  - une pince sur la ligne des panneaux solaires.
- Un réseau **WiFi 2,4 GHz** (l'ESP32-C3 ne gère pas le 5 GHz).
- Un câble USB-C **qui transmet les données** (pas un câble de charge seule).

> Seul le **Shelly EM Gen1** est géré tel quel (API `/status`, authentification HTTP Basic). Les Shelly Gen2 / Gen3 utilisent une autre API (`/rpc/...`) et une authentification Digest : le code devrait être adapté.

## Prérequis logiciels

### 1. IDE

- **Arduino IDE 2.x** : <https://www.arduino.cc/en/software>

### 2. Gestionnaire de cartes (firmware / paquet ESP32)

Le programme est téléversé directement sur la carte, il n'y a pas de firmware à flasher séparément. Il faut en revanche installer le **paquet de cartes ESP32** :

1. **Fichier > Préférences**, dans « URL de gestionnaire de cartes supplémentaires », ajouter :
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Outils > Type de carte > Gestionnaire de cartes**, chercher **esp32** et installer le paquet d'**Espressif Systems**. La version **3.x** est recommandée (le code contient une branche de compatibilité pour la 2.0.x pour le rétroéclairage, mais elle n'a pas été testée).

### 3. Bibliothèques

À installer depuis **Outils > Gérer les bibliothèques** :

| Bibliothèque | Auteur | Version | Usage |
|---|---|---|---|
| **GFX Library for Arduino** | moononournation | dernière version | Pilote de l'écran GC9A01 et dessin |
| **ArduinoJson** | Benoit Blanchon | **7.x** (obligatoire, le code utilise `JsonDocument`) | Lecture des réponses du Shelly, d'Open-Meteo et de PokeAPI |
| **PNGdec** | Larry Bank | dernière version | Décodage des images de Pokémon |

Les bibliothèques `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Wire` et `Preferences` sont fournies avec le paquet ESP32.

### 4. Firmware du Shelly

- **Shelly EM Gen1**, firmware **1.14.0** (version avec laquelle ce projet a été développé).
- L'API locale du Shelly doit être accessible depuis votre réseau. Il est conseillé de lui **réserver une adresse IP fixe** dans votre box.
- Si la protection par identifiant est activée sur le Shelly, renseignez l'identifiant et le mot de passe dans le code (voir Configuration).

## Installation

1. Cloner le dépôt. **Le dossier du projet doit porter le même nom que le fichier `.ino`**, sinon l'IDE refuse de l'ouvrir :
   ```
   esp32-c3_monsolaire/
   ├── esp32-c3_monsolaire.ino
   └── README.md
   ```
2. Ouvrir `esp32-c3_monsolaire.ino` dans l'Arduino IDE.
3. Renseigner la configuration (voir ci-dessous).
4. Régler les options de la carte dans le menu **Outils** :

   | Option | Valeur |
   |---|---|
   | Carte | **ESP32C3 Dev Module** |
   | USB CDC On Boot | **Enabled** (nécessaire pour le moniteur série) |
   | CPU Frequency | 160 MHz |
   | Flash Size | 4MB (32Mb) |
   | Partition Scheme | Default 4MB with spiffs |
   | Upload Speed | 921600 (115200 en cas d'erreur) |
   | Port | le port série de la carte |

5. Téléverser. Si le port n'apparaît pas, maintenir le bouton **BOOT** enfoncé en branchant l'USB, puis relâcher.

## Configuration

Tout se règle en haut du fichier `esp32-c3_monsolaire.ino`, dans la section `CONFIGURATION` :

| Paramètre | Rôle |
|---|---|
| `WIFI_SSID`, `WIFI_PASS` | Votre réseau WiFi |
| `SHELLY_IP` | Adresse IP du Shelly (ex. `192.168.1.63`) |
| `SHELLY_USER`, `SHELLY_PASS` | Identifiants du Shelly. Laisser `SHELLY_PASS` vide si la protection est désactivée |
| `CH_GRID`, `CH_SOLAR` | Voie (0 ou 1) de la pince au Linky et de la pince solaire. Échangez-les si l'affichage est inversé |
| `GRID_SIGN` | `1` si la voie réseau est positive quand vous soutirez, `-1` sinon |
| `NEUTRAL_W` | Largeur de la zone neutre autour de 0 W (défaut 10) |
| `LATITUDE`, `LONGITUDE` | Position pour la météo (défaut : Bouguenais) |
| `TIMEZONE`, `NTP_SERVER` | Fuseau horaire et serveur de temps (défaut : France) |
| `POKEMON_MAX` | Les Pokémon sont tirés entre 1 et cette valeur (151 = première génération) |
| `PAGE_DURATION_MS` | Durée de chaque écran en ms (`0` = pas de défilement automatique pour cet écran) |
| `REFRESH_MS`, `WEATHER_REFRESH_MS` | Fréquence de lecture du Shelly (2 s) et de la météo (10 min) |

D'autres réglages se trouvent plus bas : les maxima des jauges de l'écran énergie (`MAX_SOLAR_W`, `MAX_HOUSE_W`, `MAX_GRID_W`), les couleurs de l'écran cumul (`COL_SOLAR`, `COL_HOUSE`, `COL_IMPORT`, `COL_EXPORT`) et l'animation de démarrage (`BOOT_TITLE`, `BOOT_MIN_MS`). Le titre de l'animation doit rester à **6 lettres**, car sa position est calculée pour cette longueur.

### Brochage utilisé

| Fonction | Broche ESP32-C3 |
|---|---|
| Écran : DC / CS / SCK / MOSI | 2 / 10 / 6 / 7 |
| Rétroéclairage | 3 |
| Tactile : SDA / SCL / RST | 4 / 5 / 1 (adresse I2C `0x15`) |

Ces broches correspondent à la carte ESP32-2424S012C. Si l'écran reste noir ou si le tactile ne répond pas, vérifiez-les avec la fiche de votre fournisseur.

### Ne publiez pas vos identifiants

Le mot de passe WiFi et celui du Shelly sont écrits en clair dans le code. **Avant de publier sur GitHub, remplacez-les par des valeurs fictives** (comme `MON_WIFI` / `MON_MOT_DE_PASSE`). Une bonne pratique est de les déplacer dans un fichier `secrets.h` ajouté au `.gitignore`.

## Comment sont calculées les valeurs

- **Réseau (Linky)** : puissance de la pince au compteur. Positive = soutirage, négative = injection.
- **Production solaire** : valeur absolue de la puissance de la pince solaire.
- **Consommation de la maison** : `production solaire + réseau`.
- **Cumul du jour** : le Shelly ne fournit que des compteurs qui montent depuis sa mise en service. Le programme mémorise leur valeur à chaque minuit (en mémoire flash, donc conservée en cas de redémarrage) et affiche la différence.
  - consommation du jour = production consommée sur place + énergie soutirée ;
  - **autoconsommation** = part de la production utilisée dans la maison ;
  - **autonomie** = part de la consommation couverte par le solaire.

Limites : si le module est éteint à minuit, le cumul du nouveau jour compte à partir de son redémarrage. Si le Shelly est injoignable à minuit, l'énergie consommée pendant la panne n'est comptée dans aucun des deux jours.

## Services externes utilisés

| Service | Usage | Remarque |
|---|---|---|
| Shelly (réseau local) | Puissances et compteurs | Aucun cloud nécessaire |
| [Open-Meteo](https://open-meteo.com) | Météo, lever et coucher du soleil | Gratuit, sans clé API, usage non commercial. Données sous licence CC BY 4.0 : mention « Weather data by Open-Meteo.com » |
| [PokeAPI](https://pokeapi.co) et son dépôt d'images | Types et sprites des Pokémon | Gratuit, sans clé API |
| `pool.ntp.org` | Heure | |

Les connexions HTTPS sont établies **sans vérification du certificat** (`setInsecure()`), ce qui est acceptable ici car aucune donnée sensible n'est échangée.

## Dépannage

| Symptôme | Piste |
|---|---|
| La carte n'apparaît pas dans l'IDE | Installer le paquet **esp32** d'Espressif et choisir **ESP32C3 Dev Module** |
| Aucun port série | Changer de câble (données), maintenir BOOT en branchant, activer **USB CDC On Boot** |
| Écran noir | Vérifier le brochage et la broche du rétroéclairage |
| « Shelly injoignable » | Vérifier l'IP, que le Shelly est sur le même réseau, et tester `http://IP/status` dans un navigateur. `HTTP 401` dans le moniteur série = identifiants incorrects |
| Production et réseau inversés | Échanger `CH_GRID` et `CH_SOLAR` |
| Le réseau est négatif quand je consomme | Mettre `GRID_SIGN` à `-1` |
| Heure à `--:--` ou « Cumul en attente » | La carte n'a pas encore accès à Internet (synchronisation de l'heure) |
| « Pokemon chargement » qui dure | Vérifier l'accès à Internet. Un nouveau Pokémon est téléchargé à chaque passage, ce qui prend quelques secondes |
| Erreur de compilation sur `JsonDocument` | Installer **ArduinoJson 7.x** (la 6.x n'est pas compatible) |

Le moniteur série (115200 bauds) affiche les valeurs lues (`solaire=... reseau=...`), les erreurs HTTP et un message à chaque remise à zéro quotidienne du cumul.

## Licence et crédits
- Les noms et images de Pokémon appartiennent à Nintendo, Game Freak et The Pokémon Company. Ils sont récupérés via PokeAPI pour un usage personnel et non commercial.
- Bibliothèques : GFX Library for Arduino, ArduinoJson, PNGdec (voir leurs licences respectives).
