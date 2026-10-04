/*
 *	PearPC
 *	ata.cc
 *
 *	Copyright (C) 2003, 2004 Sebastian Biallas (sb@biallas.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <cstdio>
#include <cstring>
#include <errno.h>

#include "debug/tracers.h"
#include "ata.h"

#include "tools/snprintf.h"

ATADevice::ATADevice(const char *name)
	: IDEDevice(name)
{
	setMode(ATA_DEVICE_MODE_PLAIN, 512);
}

void ATADevice::init(int aHeads, int aCyl, int aSpt)
{
	mHeads = aHeads;
	mCyl = aCyl;
	mSpt = aSpt;
}

ATADevice::~ATADevice()
{
}

uint ATADevice::getBlockSize()
{
	return 512;
}

uint ATADevice::getBlockCount()
{
	return blocks;
}

/*
 *
 */
ATADeviceFile::ATADeviceFile(const char *name, const char *filename)
	: ATADevice(name), mMmapBase(NULL), mMmapSize(0), mCurrentOffset(0)
{
	mFile = sys_fopen(filename, SYS_OPEN_READ | SYS_OPEN_WRITE);
	if (mFile) {
        const uint64 cylinderBytes = 16ULL * 63 * 512;
        const uint64 maxBytes = 65535ULL * cylinderBytes;
        int seekError = sys_fseek(mFile, 0, SYS_SEEK_END);
        FileOfs fileSize = sys_ftell(mFile);
        const char *error = NULL;
        if (seekError || fileSize == (FileOfs)-1 || fileSize == 0 || (fileSize % 512)) {
            error = "invalid raw disk size (must be nonzero and a multiple of 512 bytes)";
        } else if ((uint64)fileSize > maxBytes) {
            error = "raw disk exceeds the supported capacity (65535 cylinders of 16 heads and 63 sectors)";
        }
        if (error) {
            sys_fclose(mFile);
            mFile = NULL;
            setError(error);
        } else {
            uint64 size = (uint64)fileSize;
            blocks = size / 512;
            // CHS describes complete cylinders; LBA exposes every sector,
            // including the tail of an image such as an exact 8 GiB disk.
            // Preserve the historical geometry of cylinder-aligned images.
            if (size >= cylinderBytes) {
                init(16, size / cylinderBytes, 63);
            } else {
                init(1, blocks, 1);
            }
            mMmapSize = size;
            mMmapBase = (byte *)sys_mmap_file(mFile, size, false /* read-write */);
        }
	} else {
		char buf[256];
		ht_snprintf(buf, sizeof buf, "%s: could not open file (%s)", filename, strerror(errno));
		setError(buf);
	}
}

ATADeviceFile::~ATADeviceFile()
{
	if (mMmapBase) {
		sys_msync_file(mMmapBase, mMmapSize);
		sys_munmap_file(mMmapBase, mMmapSize);
		mMmapBase = NULL;
	}
	if (mFile) {
		sys_fclose(mFile);
		mFile = NULL;
	}
}

bool ATADeviceFile::seek(uint64 blockno)
{
	mCurrentOffset = 512 * (uint64)blockno;
	if (!mMmapBase && mFile) {
		sys_fseek(mFile, mCurrentOffset);
	}
	return true;
}

void ATADeviceFile::flush()
{
	if (mMmapBase) {
		sys_msync_file(mMmapBase, mMmapSize);
	}
	if (mFile) {
		sys_flush(mFile);
	}
}

int ATADeviceFile::read(byte *buf, int size)
{
	if (mSectorFirst && mSectorFirst < mSectorSize) {
		return IDEDevice::read(buf, size);
	}
	if (mMode == ATA_DEVICE_MODE_PLAIN) {
		if (mMmapBase && (mCurrentOffset + size <= mMmapSize)) {
			memcpy(buf, mMmapBase + mCurrentOffset, size);
			mCurrentOffset += size;
			return size;
		}
		if (mFile) {
			int n = sys_pread(mFile, buf, size, mCurrentOffset);
			if (n > 0) {
				mCurrentOffset += n;
				return n;
			}
		}
	}
	return IDEDevice::read(buf, size);
}

int ATADeviceFile::write(byte *buf, int size)
{
	if (mSectorFirst && mSectorFirst < mSectorSize) {
		return IDEDevice::write(buf, size);
	}
	if (mMode == ATA_DEVICE_MODE_PLAIN) {
		if (mMmapBase && (mCurrentOffset + size <= mMmapSize)) {
			memcpy(mMmapBase + mCurrentOffset, buf, size);
			mCurrentOffset += size;
			return size;
		}
		if (mFile) {
			int n = sys_pwrite(mFile, buf, size, mCurrentOffset);
			if (n > 0) {
				mCurrentOffset += n;
				return n;
			}
		}
	}
	return IDEDevice::write(buf, size);
}

int ATADeviceFile::readBlock(byte *buf)
{
	if (mMode == ATA_DEVICE_MODE_PLAIN && mMmapBase && (mCurrentOffset + 512 <= mMmapSize)) {
		memcpy(buf, mMmapBase + mCurrentOffset, 512);
		mCurrentOffset += 512;
		return 0;
	}
	if (mMmapBase && mFile) {
		sys_fseek(mFile, mCurrentOffset);
	}
	sys_fread(mFile, buf, 512);
	mCurrentOffset += 512;
	if (mMode & ATA_DEVICE_MODE_ECC) {
		// add ECC bytes..
		IO_IDE_ERR("ATADeviceFile: ECC not implemented\n");
	}
	return 0;
}

int ATADeviceFile::writeBlock(byte *buf)
{
	if (mMode == ATA_DEVICE_MODE_PLAIN && mMmapBase && (mCurrentOffset + 512 <= mMmapSize)) {
		memcpy(mMmapBase + mCurrentOffset, buf, 512);
		mCurrentOffset += 512;
		return 0;
	}
	if (mMmapBase && mFile) {
		sys_fseek(mFile, mCurrentOffset);
	}
	sys_fwrite(mFile, buf, 512);
	mCurrentOffset += 512;
	return 0;
}

bool ATADeviceFile::promSeek(FileOfs pos)
{
	mCurrentOffset = pos;
	if (!mMmapBase && mFile) {
		return sys_fseek(mFile, pos) == 0;
	}
	return true;
}

uint ATADeviceFile::promRead(byte *buf, uint size)
{
	if (mMmapBase && (mCurrentOffset + size <= mMmapSize)) {
		memcpy(buf, mMmapBase + mCurrentOffset, size);
		mCurrentOffset += size;
		return size;
	}
	if (mFile) {
		uint r = sys_fread(mFile, buf, size);
		mCurrentOffset += r;
		return r;
	}
	return 0;
}


