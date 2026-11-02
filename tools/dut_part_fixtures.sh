#!/bin/sh
# Fixtures for tools/dut_part_logic_test.c, made by ESP-IDF's own generators:
# an ESP32-C6 Arduino-style partition table, a SPIFFS image, FAT images with
# 512 and 4096-byte sectors, a FAT16 one, and a wear-levelled FAT.
#   IDF_PATH=... PYTHON=<IDF python env> tools/dut_part_fixtures.sh <out dir>
set -e
out=${1:?output directory}
py=${PYTHON:-python3}
c=${IDF_PATH:?IDF_PATH}/components
mkdir -p "$out/src/sub"
cd "$out"
printf '{"io_username":"placeholder"}\n' > src/secrets.json
printf 'hi\n' > src/sub/a.txt
cat > c6.csv <<'CSV'
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1E0000,
app1,     app,  ota_1,   0x1F0000,0x1E0000,
spiffs,   data, spiffs,  0x3D0000,0x20000,
coredump, data, coredump,0x3F0000,0x10000,
CSV
"$py" "$c/partition_table/gen_esp32part.py" c6.csv ptable.bin
"$py" "$c/spiffs/spiffsgen.py" 0x20000 src spiffs.bin
"$py" "$c/fatfs/fatfsgen.py" src --output_file fat512.img --partition_size 1048576 --sector_size 512 --long_name_support
"$py" "$c/fatfs/fatfsgen.py" src --output_file fat4k.img --partition_size 1048576 --sector_size 4096 --long_name_support
"$py" "$c/fatfs/fatfsgen.py" src --output_file fat16.img --partition_size 8388608 --sector_size 512 --long_name_support --fat_type 16
"$py" "$c/fatfs/wl_fatfsgen.py" src --output_file fatwl.img --partition_size 1048576 --long_name_support
