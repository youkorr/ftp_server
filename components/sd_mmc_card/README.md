# esphome_sd_card

SD MMC cards components for esphome.

## Config

```yaml
sd_mmc_card:
  id: sd_mmc_card
  mode_1bit: false
  clk_pin: GPIO14
  cmd_pin: GPIO15
  data0_pin: GPIO2
  data1_pin: GPIO4
  data2_pin: GPIO12
  data3_pin: GPIO13
  power_ctrl_pin: GPIO43  # Optionnel : GPIO pour contrôler l'alimentation de la carte SD
```

* **mode_1bit** (Optional, bool): spécifie si le mode 1 bit ou 4 bits est utilisé
* **clk_pin** : (Required, GPIO): broche d'horloge
* **cmd_pin** : (Required, GPIO): broche de commande
* **data0_pin**: (Required, GPIO): broche de données 0
* **data1_pin**: (Optional, GPIO): broche de données 1, utilisée uniquement en mode 4 bits
* **data2_pin**: (Optional, GPIO): broche de données 2, utilisée uniquement en mode 4 bits
* **data3_pin**: (Optional, GPIO): broche de données 3, utilisée uniquement en mode 4 bits
* **power_ctrl_pin**: (Optional, GPIO): broche pour contrôler l'alimentation de la carte SD (par exemple, GPIO43 pour l'ESP32-S3-Box-3)

### Contrôle d'alimentation (PWR_CTRL)

Pour les appareils comme l'ESP32-S3-Box-3, vous pouvez utiliser la broche `power_ctrl_pin` pour activer ou désactiver l'alimentation de la carte SD. Par exemple, sur l'ESP32-S3-Box-3, la broche GPIO43 est souvent utilisée pour contrôler l'alimentation du lecteur de carte SD.

Exemple de configuration pour l'ESP32-S3-Box-3 :
```yaml
sd_mmc_card:
  clk_pin: GPIO14
  cmd_pin: GPIO15
  data0_pin: GPIO2
  power_ctrl_pin: GPIO43  # Active l'alimentation du lecteur de carte SD
```
#### ESP-IDF Framework

Par défaut, les noms de fichiers longs ne sont pas activés. Pour changer ce comportement, `CONFIG_FATFS_LFN_STACK` ou `CONFIG_FATFS_LFN_HEAP` doit être défini dans la configuration du framework. Voir la [documentation Espressif](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/kconfig.html#config-fatfs-long-filenames) pour plus de détails.

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_FATFS_LFN_STACK: "y"
```


* **path** (Templatable, string): chemin absolu du fichier

### Create directory

```yaml
sd_mmc_card.create_directory:
    path: "/test"
```

Crée un dossier sur la carte SD.

* **path** (Templatable, string): chemin absolu du dossier

### Remove directory

Supprime un dossier de la carte SD.

```yaml
sd_mmc_card.remove_directory:
    path: "/test"
```

## Sensors

### Used space

```yaml
sensor:
  - platform: sd_mmc_card
    type: used_space
    name: "SD card used space"
```

Espace utilisé sur la carte SD en octets.

* Toutes les options [sensor](https://esphome.io/components/sensor/) sont disponibles

### Total space

```yaml
sensor:
  - platform: sd_mmc_card
    type: total_space
    name: "SD card total space"
```

Capacité totale de la carte SD.

* Toutes les options [sensor](https://esphome.io/components/sensor/) sont disponibles

### Free space

```yaml
sensor:
  - platform: sd_mmc_card
    type: free_space
    name: "SD card free space"
```

Espace libre sur la carte SD.

* Toutes les options [sensor](https://esphome.io/components/sensor/) sont disponibles

### File size

```yaml
sensor:
  - platform: sd_mmc_card
    type: file_size
    path: "/test.txt"
```

Retourne la taille du fichier spécifié.

* **path** (Required, string): chemin du fichier
* Toutes les options [sensor](https://esphome.io/components/sensor/) sont disponibles

## Text Sensor

```yaml
text_sensor:
  - platform: sd_mmc_card
    sd_card_type:
      name: "SD card type"
```

Type de carte SD (MMC, SDSC, ...)

* Toutes les options [text sensor](https://esphome.io/components/text_sensor/) sont disponibles


```
