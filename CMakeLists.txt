cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(boss_footswitch)

spiffs_create_partition_image(web_assets data FLASH_IN_PROJECT)
