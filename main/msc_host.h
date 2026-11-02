#ifndef MSC_HOST_H
#define MSC_HOST_H

/*
 * A device's USB mass storage, read and written by the bridge itself:
 * Bulk-Only Transport + SCSI over the USB host backend, FAT through FatFS.
 * For UF2 bootloaders (copy a .uf2 to flash a board), CIRCUITPY drives and
 * other small FAT volumes: list, download, upload/replace, delete, rename,
 * mkdir.  One volume is mounted at a time, only for the duration of a call.
 *
 * Quick operations answer directly; copies run as rescue jobs (the HTTP
 * server is single-threaded).
 */

#include "cJSON.h"
#include "rescue_job.h"

/* GET: inquiry, capacity, volume label, free space and the root listing. */
int msc_info(const char *busid, cJSON **out);
/* GET ?path=/dir */
int msc_list(const char *busid, const char *path, cJSON **out);
/* POST {path} / {from, to} / {path} */
int msc_delete(const char *busid, const cJSON *args, cJSON **out);
int msc_rename(const char *busid, const cJSON *args, cJSON **out);
int msc_mkdir(const char *busid, const cJSON *args, cJSON **out);

/* Jobs.  write: {"file": <staged name>, "path": "/FIRMWARE.UF2"}: copy a
   staged file to the volume (replacing an existing file).  uf2: the same
   with the bootloader entered first when the device has no mass storage
   yet, and a device that reboots after the last block counted as success.
   read: {"path": "/boot_out.txt", "file": <staged name>}: copy a file off
   the volume into the staged files (download it with GET /api/rescue/files). */
int msc_write_job(rescue_job_t *job, const char *busid, const cJSON *args);
int msc_uf2_job(rescue_job_t *job, const char *busid, const cJSON *args);
int msc_read_job(rescue_job_t *job, const char *busid, const cJSON *args);

#endif
