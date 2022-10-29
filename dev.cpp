#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "dev.hpp"

extern bool ioSync;
extern bool ioDirect;

#define BLKBSZGET  0x80081270
#define BLKPBSZGET 0x127b
#define BLKIOMIN   0x1278
#define BLKIOOPT   0x1279

Dev::Dev(const char *path) {
	int flags =             O_RDWR|O_NOCTTY;
	if (ioSync)   {flags |= O_RSYNC|O_SYNC; /*fputs("Use synchronous I/O\n", stderr);*/}
	if (ioDirect) {flags |= O_DIRECT; /*fputs("Use direct I/O\n", stderr);*/}
	this->fd = open(path, flags);
	if (this->fd < 0) {perror("Error opening Dev"); return;}
	
	if (ioctl(this->fd, BLKPBSZGET, &this->blkSize) < 0) {this->blkSize = 0; perror("BLKPBSZGET"); return;}
	if (!this->blkSize) {this->blkSize = 0;}
}

Dev::~Dev() {
	if (close(this->fd) != 0) {perror("Error closing Dev");}
	this->fd = -1;
}

ssize_t Dev::write(uint64_t blkNo, const void *buf, size_t count) {
	return ::pwrite(this->fd, buf, count, blkNo*this->blkSize);
}

ssize_t Dev::read(uint64_t blkNo, void *buf, size_t count) {
	return ::pread(this->fd, buf, count, blkNo*this->blkSize);
}

bool Dev::isOpen() {return ((this->fd >= 0) && (this->blkSize > 0));}
uint64_t Dev::getBlkSize() {return this->blkSize;}
void Dev::dontNeed() {posix_fadvise(this->fd, 0, 0, POSIX_FADV_DONTNEED);}
void Dev::sync() {fsync(this->fd);}