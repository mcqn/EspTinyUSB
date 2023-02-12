
set(LIBRARY_SRCS
  src/esptinyusb.cpp
  src/device/web/webusb.cpp
  src/device/hid/hidgeneric.cpp
  src/device/hid/hidcomposite.cpp
  src/device/hid/hidmouse.cpp
  src/device/hid/hidusb.cpp
  src/device/hid/hidgamepad.cpp
  src/device/hid/hidkeyboard.cpp
  src/device/dfu/dfuusb.cpp
  src/device/msc/flashdisk.cpp
  src/device/msc/mscusb.cpp
  src/device/msc/sdcard.cpp
  src/device/msc/ramdisk.cpp
  src/device/midi/midiusb.cpp
  src/device/cdc/cdcusb.cpp
  src/usb_descriptors.cpp
  src/host/msc/usb_msc.cpp_backup
  src/host/msc/vfs/diskio_rawmsc.cpp
  src/host/msc/usb_msc.cpp
  src/host/acm/usb_acm.cpp
  src/host/common/usb_host.cpp
  src/host/common/usb_device.cpp
  )

set(includedirs
  src
  ../arduino/cores/esp32
  ../arduino/variants/esp32s2
  )

set(srcs ${LIBRARY_SRCS})
#set(requires spi_flash mbedtls mdns esp_adc_cal wifi_provisioning nghttp wpa_supplicant)
set(requires spi_flash wpa_supplicant usb arduino fatfs spiffs)
#set(priv_requires fatfs nvs_flash app_update spiffs bootloader_support openssl bt esp_ipc esp_hid)

idf_component_register(INCLUDE_DIRS ${includedirs} SRCS ${srcs} REQUIRES ${requires}) # PRIV_REQUIRES ${priv_requires})

#target_compile_options(${COMPONENT_TARGET} PUBLIC
#    -DARDUINO=10812
#    -DARDUINO_${idf_target_for_macro}_DEV
#    -DARDUINO_ARCH_ESP32
#    -DARDUINO_BOARD="${idf_target_caps}_DEV"
#    -DARDUINO_VARIANT="${CONFIG_ARDUINO_VARIANT}"
#    -DESP32)

#if(IDF_TARGET MATCHES "esp32s2|esp32s3" AND CONFIG_TINYUSB_ENABLED)
#    maybe_add_component(arduino_tinyusb)
#endif()
