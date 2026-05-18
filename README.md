# fat32filesystem

`fat32filesystem` contains the FAT32 filesystem implementation and supporting parsing/path/cache code extracted from RPIBareMetalOS.

Current scope:

- FAT32 core implementation (`fat32_*`)
- Filesystem support modules used by FAT32 (`filesystem_errors`, `filesystem_path`, `partition`, `master_boot_record`, `file_map`)

OS integration (`filesystems.cpp`, entity registry wiring, device bootstrap) remains in RPIBareMetalOS.
