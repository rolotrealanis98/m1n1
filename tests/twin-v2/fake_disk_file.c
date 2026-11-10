/* SPDX-License-Identifier: MIT */
/* offline-twin-20261109: the fake ANS's durable NAND = a sparse backing file of 4096-byte
 * sectors (F_NSZE sectors). Raw block read/write primitives only; the copied fake_ans.c is
 * patched (build-native-model.sh) to route its media reads/persist through these:
 *   r3.1 (ro): fake_disk_block() -> fake_file_read_block()
 *   r4   (rw): base_block() -> file; persist_cache() -> fake_file_write_block(); the in-memory
 *              ov_cache stays as the volatile write cache (crash-before-flush = power loss).
 * Reads past EOF / holes are zeros. */
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
static int disk_fd = -1;
static bool disk_rw;
unsigned long long fake_disk_file_reads, fake_disk_file_writes, fake_disk_file_errors;
int fake_disk_open(const char *path, bool writable)
{
    disk_fd = open(path, writable ? O_RDWR : O_RDONLY);
    disk_rw = writable;
    return disk_fd;
}
void fake_file_read_block(uint64_t lba, uint8_t *out)
{
    for (int i = 0; i < 4096; i++) out[i] = 0;
    fake_disk_file_reads++;
    if (disk_fd < 0) { fake_disk_file_errors++; return; }
    if (pread(disk_fd, out, 4096, (off_t)(lba * 4096)) < 0) fake_disk_file_errors++;
}
bool fake_file_write_block(uint64_t lba, const uint8_t *in)
{
    if (disk_fd < 0 || !disk_rw) { fake_disk_file_errors++; return false; }
    fake_disk_file_writes++;
    return pwrite(disk_fd, in, 4096, (off_t)(lba * 4096)) == 4096;
}
bool fake_disk_sync(void) { return disk_fd >= 0 && (!disk_rw || fsync(disk_fd) == 0); }
/* twin v2: create a new sparse image (O_EXCL) of `bytes` and keep it open read-write. */
int fake_disk_create(const char *path, unsigned long long bytes)
{
    disk_fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    disk_rw = disk_fd >= 0;
    if (disk_fd >= 0 && ftruncate(disk_fd, (off_t)bytes)) return -1;
    return disk_fd;
}
