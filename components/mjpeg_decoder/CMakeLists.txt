# CMakeLists.txt pour le composant MJPEG
cmake_minimum_required(VERSION 3.5)

idf_component_register(
    SRCS 
        "mjpeg_decoder.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        "esp_timer"
        "driver"
        "freertos"
        "spiffs"
)

# Exemple de structure de fichiers pour le composant
# mjpeg_decoder/
# ├── CMakeLists.txt
# ├── include/
# │   └── mjpeg_decoder.h
# ├── mjpeg_decoder.c
# └── component.mk

# component.mk (pour la compatibilité avec l'ancienne méthode de build)
COMPONENT_ADD_INCLUDEDIRS := include
COMPONENT_SRCDIRS := .
