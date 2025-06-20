/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <TargetConditionals.h>
#include "Defines.h"
#if TARGET_OS_EXCLAVEKIT
  #define OSSwapBigToHostInt32 __builtin_bswap32
  #define OSSwapBigToHostInt64 __builtin_bswap64
  #define htonl                __builtin_bswap32
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/errno.h>
  #include <sys/fcntl.h>
  #include <unistd.h>
  #include <mach/host_info.h>
  #include <mach/mach.h>
  #include <mach/mach_host.h>
#if SUPPORT_CLASSIC_RELOCS
  #include <mach-o/reloc.h>
  #include <mach-o/x86_64/reloc.h>
#endif
extern "C" {
//  #include <corecrypto/ccdigest.h>
//  #include <corecrypto/ccsha1.h>
//  #include <corecrypto/ccsha2.h>
}
#endif




#include "Defines.h"

#include <mach-o/nlist.h>

#if !BUILDING_DYLD
  #include <vector>
#endif // !BUILDING_DYLD

#include "Architecture.h"
#include "Array.h"
#include "Header.h"
#include "MachOFile.h"
#include "Platform.h"
#include "SupportedArchs.h"
#include "CodeSigningTypes.h"

#include "ObjC.h"

#if (BUILDING_DYLD || BUILDING_LIBDYLD) && !TARGET_OS_EXCLAVEKIT
    #include <subsystem.h>
#endif

#if !BUILDING_DYLD
#include "ObjCVisitor.h"
#endif

using mach_o::Header;
using mach_o::Platform;

namespace dyld3 {

#if !TARGET_OS_EXCLAVEKIT

////////////////////////////  posix wrappers ////////////////////////////////////////

// <rdar://problem/10111032> wrap calls to stat() with check for EAGAIN
int stat(const char* path, struct stat* buf)
{
    int result;
    do {
#if BUILDING_DYLD
        result = ::stat_with_subsystem(path, buf);
#else
        result = ::stat(path, buf);
#endif
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}

// <rdar://problem/10111032> wrap calls to stat() with check for EAGAIN
int fstatat(int fd, const char *path, struct stat *buf, int flag)
{
    int result;
    do {
        result = ::fstatat(fd, path, buf, flag);
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}

// <rdar://problem/13805025> dyld should retry open() if it gets an EGAIN
int open(const char* path, int flag, int other)
{
    int result;
    do {
#if BUILDING_DYLD
        if (flag & O_CREAT)
            result = ::open(path, flag, other);
        else
            result = ::open_with_subsystem(path, flag);
#else
        result = ::open(path, flag, other);
#endif
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}
#endif // !TARGET_OS_EXCLAVEKIT


////////////////////////////  FatFile ////////////////////////////////////////

const FatFile* FatFile::isFatFile(const void* fileStart)
{
    const FatFile* fileStartAsFat = (FatFile*)fileStart;
    if ( (fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC)) || (fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return fileStartAsFat;
    else
        return nullptr;
}

bool FatFile::isValidSlice(Diagnostics& diag, uint64_t fileLen, uint32_t sliceIndex,
                           uint32_t sliceCpuType, uint32_t sliceCpuSubType, uint64_t sliceOffset, uint64_t sliceLen) const {
    if ( greaterThanAddOrOverflow(sliceOffset, sliceLen, fileLen) ) {
        diag.error("slice %d extends beyond end of file", sliceIndex);
        return false;
    }
    const dyld3::MachOFile* mf = (const dyld3::MachOFile*)((uint8_t*)this+sliceOffset);
    if (!mf->isMachO(diag, sliceLen))
        return false;
    if ( mf->cputype != (cpu_type_t)sliceCpuType ) {
        diag.error("cpu type in slice (0x%08X) does not match fat header (0x%08X)", mf->cputype, sliceCpuType);
        return false;
    }
    else if ( (mf->cpusubtype & ~CPU_SUBTYPE_MASK) != (sliceCpuSubType & ~CPU_SUBTYPE_MASK) ) {
        diag.error("cpu subtype in slice (0x%08X) does not match fat header (0x%08X)", mf->cpusubtype, sliceCpuSubType);
        return false;
    }
    uint32_t pageSizeMask = mf->uses16KPages() ? 0x3FFF : 0xFFF;
    if ( (sliceOffset & pageSizeMask) != 0 ) {
        // slice not page aligned
        if ( strncmp((char*)this+sliceOffset, "!<arch>", 7) == 0 )
            diag.error("file is static library");
        else
            diag.error("slice is not page aligned");
        return false;
    }
    return true;
}

void FatFile::forEachSlice(Diagnostics& diag, uint64_t fileLen, bool validate,
                           void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const
{
    if ( this->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
        const uint64_t maxArchs = ((4096 - sizeof(fat_header)) / sizeof(fat_arch));
        const uint32_t numArchs = OSSwapBigToHostInt32(nfat_arch);
        if ( numArchs > maxArchs ) {
            diag.error("fat header too large: %u entries", numArchs);
            return;
        }
        // <rdar://90700132> make sure architectures list doesn't exceed the file size
        // We can’t overflow due to maxArch check
        // Check numArchs+1 to cover the extra read after the loop
        if ( (sizeof(fat_header) + ((numArchs + 1) * sizeof(fat_arch))) > fileLen ) {
            diag.error("fat header malformed, architecture slices extend beyond end of file");
            return;
        }
        bool stop = false;
        const fat_arch* const archs = (fat_arch*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; i < numArchs; ++i) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
            uint32_t offset     = OSSwapBigToHostInt32(archs[i].offset);
            uint32_t len        = OSSwapBigToHostInt32(archs[i].size);
            Diagnostics sliceDiag;
            if ( !validate || isValidSlice(sliceDiag, fileLen, i, cpuType, cpuSubType, offset, len) )
                callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            if ( stop )
                break;
            if ( sliceDiag.hasError() )
                diag.appendError("%s, ", sliceDiag.errorMessageCStr());
        }

        // Look for one more slice
        if ( numArchs != maxArchs ) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[numArchs].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[numArchs].cpusubtype);
            uint32_t offset     = OSSwapBigToHostInt32(archs[numArchs].offset);
            uint32_t len        = OSSwapBigToHostInt32(archs[numArchs].size);
            if ((cpuType == CPU_TYPE_ARM64) && ((cpuSubType == CPU_SUBTYPE_ARM64_ALL || cpuSubType == CPU_SUBTYPE_ARM64_V8))) {
                if ( !validate || isValidSlice(diag, fileLen, numArchs, cpuType, cpuSubType, offset, len) )
                    callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            }
        }
    }
    else if ( this->magic == OSSwapBigToHostInt32(FAT_MAGIC_64) ) {
        const uint32_t numArchs = OSSwapBigToHostInt32(nfat_arch);
        if ( numArchs > ((4096 - sizeof(fat_header)) / sizeof(fat_arch_64)) ) {
            diag.error("fat header too large: %u entries", OSSwapBigToHostInt32(nfat_arch));
            return;
        }
        // <rdar://90700132> make sure architectures list doesn't exceed the file size
        // We can’t overflow due to maxArch check
        if ( (sizeof(fat_header) + (numArchs * sizeof(fat_arch_64))) > fileLen ) {
            diag.error("fat header malformed, architecture slices extend beyond end of file");
            return;
        }
        bool stop = false;
        const fat_arch_64* const archs = (fat_arch_64*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; i < numArchs; ++i) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
            uint64_t offset     = OSSwapBigToHostInt64(archs[i].offset);
            uint64_t len        = OSSwapBigToHostInt64(archs[i].size);
            if ( !validate || isValidSlice(diag, fileLen, i, cpuType, cpuSubType, offset, len) )
                callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            if ( stop )
                break;
        }
    }
    else {
        diag.error("not a fat file");
    }
}

void FatFile::forEachSlice(Diagnostics& diag, uint64_t fileLen, void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const
{
    forEachSlice(diag, fileLen, true, callback);
}

const char* FatFile::archNames(char strBuf[256], uint64_t fileLen) const
{
    strBuf[0] = '\0';
    Diagnostics   diag;
    __block bool  needComma = false;
    this->forEachSlice(diag, fileLen, false, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
        if ( needComma )
            strlcat(strBuf, ",", 256);
        strlcat(strBuf, mach_o::Architecture(sliceCpuType, sliceCpuSubType).name(), 256);
        needComma = true;
    });
    return strBuf;
}

bool FatFile::isFatFileWithSlice(Diagnostics& diag, uint64_t fileLen, const GradedArchs& archs, bool isOSBinary,
                                 uint64_t& sliceOffset, uint64_t& sliceLen, bool& missingSlice) const
{
    missingSlice = false;
    if ( (this->magic != OSSwapBigToHostInt32(FAT_MAGIC)) && (this->magic != OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return false;

    __block int bestGrade = 0;
    forEachSlice(diag, fileLen, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
        if (int sliceGrade = archs.grade(sliceCpuType, sliceCpuSubType, isOSBinary)) {
            if ( sliceGrade > bestGrade ) {
                sliceOffset = (char*)sliceStart - (char*)this;
                sliceLen    = sliceSize;
                bestGrade   = sliceGrade;
            }
        }
    });
    if ( diag.hasError() )
        return false;

    if ( bestGrade == 0 )
        missingSlice = true;

    return (bestGrade != 0);
}


////////////////////////////  GradedArchs ////////////////////////////////////////


#define GRADE_i386        CPU_TYPE_I386,       CPU_SUBTYPE_I386_ALL,    false
#define GRADE_x86_64      CPU_TYPE_X86_64,     CPU_SUBTYPE_X86_64_ALL,  false
#define GRADE_x86_64h     CPU_TYPE_X86_64,     CPU_SUBTYPE_X86_64_H,    false
#define GRADE_armv7       CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7,      false
#define GRADE_armv7s      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7S,     false
#define GRADE_armv7k      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7K,     false
#define GRADE_armv6m      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V6M,     false
#define GRADE_armv7m      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7M,     false
#define GRADE_armv7em     CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7EM,    false
#define GRADE_armv8m      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V8M,     false
#define GRADE_arm64       CPU_TYPE_ARM64,      CPU_SUBTYPE_ARM64_ALL,   false
#define GRADE_arm64e      CPU_TYPE_ARM64,      CPU_SUBTYPE_ARM64E,      false
#define GRADE_arm64e_pb   CPU_TYPE_ARM64,      CPU_SUBTYPE_ARM64E,      true
#define GRADE_arm64_32    CPU_TYPE_ARM64_32,   CPU_SUBTYPE_ARM64_32_V8, false
const GradedArchs GradedArchs::i386              = GradedArchs({GRADE_i386,    1});
const GradedArchs GradedArchs::x86_64            = GradedArchs({GRADE_x86_64,  1});
const GradedArchs GradedArchs::x86_64h           = GradedArchs({GRADE_x86_64h, 2}, {GRADE_x86_64, 1});
const GradedArchs GradedArchs::arm64             = GradedArchs({GRADE_arm64,   1});
#if SUPPORT_ARCH_arm64e
const GradedArchs GradedArchs::arm64e_keysoff    = GradedArchs({GRADE_arm64e,    2}, {GRADE_arm64, 1});
const GradedArchs GradedArchs::arm64e_keysoff_pb = GradedArchs({GRADE_arm64e_pb, 2}, {GRADE_arm64, 1});
const GradedArchs GradedArchs::arm64e            = GradedArchs({GRADE_arm64e,    1});
const GradedArchs GradedArchs::arm64e_pb         = GradedArchs({GRADE_arm64e_pb, 1});
#endif
const GradedArchs GradedArchs::armv7             = GradedArchs({GRADE_armv7,   1});
const GradedArchs GradedArchs::armv7s            = GradedArchs({GRADE_armv7s,  2}, {GRADE_armv7, 1});
const GradedArchs GradedArchs::armv7k            = GradedArchs({GRADE_armv7k,  1});
const GradedArchs GradedArchs::armv7m            = GradedArchs({GRADE_armv7m,  1});
const GradedArchs GradedArchs::armv7em           = GradedArchs({GRADE_armv7em,  1});


#if SUPPORT_ARCH_arm64_32
const GradedArchs GradedArchs::arm64_32          = GradedArchs({GRADE_arm64_32, 1});
#endif

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
const GradedArchs GradedArchs::launch_AS         = GradedArchs({GRADE_arm64e,  3}, {GRADE_arm64,  2}, {GRADE_x86_64, 1});
const GradedArchs GradedArchs::launch_AS_Sim     = GradedArchs({GRADE_arm64,   2}, {GRADE_x86_64, 1});
const GradedArchs GradedArchs::launch_Intel_h    = GradedArchs({GRADE_x86_64h, 3}, {GRADE_x86_64, 2}, {GRADE_i386, 1});
const GradedArchs GradedArchs::launch_Intel      = GradedArchs({GRADE_x86_64,  2}, {GRADE_i386,   1});
const GradedArchs GradedArchs::launch_Intel_Sim  = GradedArchs({GRADE_x86_64,  2}, {GRADE_i386,   1});
#endif

int GradedArchs::grade(uint32_t cputype, uint32_t cpusubtype, bool isOSBinary) const
{
    for (const auto& p : _orderedCpuTypes) {
        if (p.type == 0) { break; }
        if ( (p.type == cputype) && (p.subtype == (cpusubtype & ~CPU_SUBTYPE_MASK)) ) {
            if ( p.osBinary ) {
                if ( isOSBinary )
                    return p.grade;
            }
            else {
                return p.grade;
            }
        }
    }
    return 0;
}

const char* GradedArchs::name() const
{
    mach_o::Architecture arch = mach_o::Architecture(_orderedCpuTypes[0].type, _orderedCpuTypes[0].subtype);
    // FIXME: Existing clients of this function don't expect the various arm64e names,
    //        such as arm64e.old.
    if ( arch.usesArm64AuthPointers() )
        return "arm64e";
    return arch.name();
}

void GradedArchs::forEachArch(bool platformBinariesOnly, void (^handler)(const char*)) const
{
    for (const auto& p : _orderedCpuTypes) {
        if (p.type == 0)
            break;
        if ( p.osBinary && !platformBinariesOnly )
            continue;
        // Note: mach_o::Architecture uses high bits to distiguish arm64e variant
        // passing the base cpu type/subtype will result in "arm64.old"
        if ( (p.type == CPU_TYPE_ARM64) && (p.subtype == CPU_SUBTYPE_ARM64E) )
            handler("arm64e");
        else
            handler(mach_o::Architecture(p.type, p.subtype).name());
    }
}

bool GradedArchs::checksOSBinary() const
{
    for (const auto& p : _orderedCpuTypes) {
        if (p.type == 0) { return false; }
        if ( p.osBinary ) { return true; }
    }
    __builtin_unreachable();
}

bool GradedArchs::supports64() const
{
    return (_orderedCpuTypes.front().type & CPU_ARCH_ABI64) != 0;
}

#if !TARGET_OS_SIMULATOR && __x86_64__
static bool isHaswell()
{
    // FIXME: figure out a commpage way to check this
    struct host_basic_info info;
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    mach_port_t hostPort = mach_host_self();
    kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), hostPort);
    return (result == KERN_SUCCESS) && (info.cpu_subtype == CPU_SUBTYPE_X86_64_H);
}
#endif

const GradedArchs& GradedArchs::forCurrentOS(bool keysOff, bool osBinariesOnly)
{
#if __arm64e__
    if ( osBinariesOnly )
        return (keysOff ? arm64e_keysoff_pb : arm64e_pb);
    else
        return (keysOff ? arm64e_keysoff : arm64e);
#elif __ARM64_ARCH_8_32__
    return arm64_32;
#elif __arm64__
    return arm64;
#elif __x86_64__
 #if TARGET_OS_SIMULATOR
    return x86_64;
  #else
    return isHaswell() ? x86_64h : x86_64;
  #endif
#else
    #error unknown platform
#endif
}

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
const GradedArchs& GradedArchs::launchCurrentOS(const char* simArches)
{
#if TARGET_OS_SIMULATOR
    // on Apple Silicon, there is both an arm64 and an x86_64 (under rosetta) simulators
    // You cannot tell if you are running under rosetta, so CoreSimulator sets SIMULATOR_ARCHS
    if ( strcmp(simArches, "arm64 x86_64") == 0 )
        return launch_AS_Sim;
    else
        return x86_64;
#elif TARGET_OS_OSX
  #if __arm64__
    return launch_AS;
  #else
    return isHaswell() ? launch_Intel_h : launch_Intel;
  #endif
#else
    // all other platforms use same grading for executables as dylibs
    return forCurrentOS(true, false);
#endif
}
#endif // BUILDING_LIBDYLD

const GradedArchs& GradedArchs::forName(const char* archName, bool keysOff)
{
    if (strcmp(archName, "x86_64h") == 0 )
        return x86_64h;
    else if (strcmp(archName, "x86_64") == 0 )
        return x86_64;
#if SUPPORT_ARCH_arm64e
    else if (strcmp(archName, "arm64e") == 0 )
        return keysOff ? arm64e_keysoff : arm64e;
#endif
    else if (strcmp(archName, "arm64") == 0 )
        return arm64;
    else if (strcmp(archName, "armv7k") == 0 )
        return armv7k;
    else if (strcmp(archName, "armv7s") == 0 )
        return armv7s;
    else if (strcmp(archName, "armv7") == 0 )
        return armv7;
    else if (strcmp(archName, "armv7m") == 0 )
        return armv7m;
    else if (strcmp(archName, "armv7em") == 0 )
        return armv7em;
#if SUPPORT_ARCH_arm64_32
    else if (strcmp(archName, "arm64_32") == 0 )
        return arm64_32;
#endif
    else if (strcmp(archName, "i386") == 0 )
        return i386;
    assert(0 && "unknown arch name");
}



////////////////////////////  MachOFile ////////////////////////////////////////

bool MachOFile::is64() const
{
    return (this->magic == MH_MAGIC_64);
}

size_t MachOFile::machHeaderSize() const
{
    return is64() ? sizeof(mach_header_64) : sizeof(mach_header);
}

uint32_t MachOFile::maskedCpuSubtype() const
{
    return (this->cpusubtype & ~CPU_SUBTYPE_MASK);
}

uint32_t MachOFile::pointerSize() const
{
    if (this->magic == MH_MAGIC_64)
        return 8;
    else
        return 4;
}

bool MachOFile::uses16KPages() const
{
    switch (this->cputype) {
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM64_32:
            return true;
        case CPU_TYPE_ARM:
            // iOS is 16k aligned for armv7/armv7s and watchOS armv7k is 16k aligned
            // HACK: Pretend armv7k kexts are 4k aligned
            if ( this->isKextBundle() )
                return false;
            return this->cpusubtype == CPU_SUBTYPE_ARM_V7K;
        default:
            return false;
    }
}

bool MachOFile::isArch(const char* aName) const
{
    return (strcmp(aName, mach_o::Architecture(this->cputype, this->cpusubtype).name()) == 0);
}

const char* MachOFile::archName() const
{
    return mach_o::Architecture(this->cputype, this->cpusubtype).name();
}

bool MachOFile::inDyldCache() const {
    return (this->flags & MH_DYLIB_IN_CACHE);
}

bool MachOFile::isDyld() const
{
    return (this->filetype == MH_DYLINKER);
}

bool MachOFile::isDyldManaged() const {
    switch ( this->filetype ) {
        case MH_BUNDLE:
        case MH_EXECUTE:
        case MH_DYLIB:
            return true;
        default:
            break;
    }
    return false;
}

bool MachOFile::isDylib() const
{
    return (this->filetype == MH_DYLIB);
}

bool MachOFile::isBundle() const
{
    return (this->filetype == MH_BUNDLE);
}

bool MachOFile::isMainExecutable() const
{
    return (this->filetype == MH_EXECUTE);
}

bool MachOFile::isDynamicExecutable() const
{
    if ( this->filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    return hasLoadCommand(LC_LOAD_DYLINKER);
}

bool MachOFile::isStaticExecutable() const
{
    if ( this->filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    return !hasLoadCommand(LC_LOAD_DYLINKER);
}

bool MachOFile::isKextBundle() const
{
    return (this->filetype == MH_KEXT_BUNDLE);
}

bool MachOFile::isFileSet() const
{
    return (this->filetype == MH_FILESET);
}

bool MachOFile::isPIE() const
{
    return (this->flags & MH_PIE);
}

bool MachOFile::isPreload() const
{
    return (this->filetype == MH_PRELOAD);
}

bool MachOFile::isMachO(Diagnostics& diag, uint64_t fileSize) const
{
    if ( fileSize < sizeof(mach_header) ) {
        diag.error("MachO header exceeds file length");
        return false;
    }

    if ( !hasMachOMagic() ) {
        // old PPC slices are not currently valid "mach-o" but should not cause an error
        if ( !hasMachOBigEndianMagic() )
            diag.error("file does not start with MH_MAGIC[_64]");
        return false;
    }
    if ( this->sizeofcmds + machHeaderSize() > fileSize ) {
        diag.error("load commands exceed length of first segment");
        return false;
    }
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) { });
    return diag.noError();
}


const MachOFile* MachOFile::isMachO(const void* content)
{
    const MachOFile* mf = (MachOFile*)content;
    if ( mf->hasMachOMagic() )
        return mf;
    return nullptr;
}

bool MachOFile::hasMachOMagic() const
{
    return ( (this->magic == MH_MAGIC) || (this->magic == MH_MAGIC_64) );
}

bool MachOFile::hasMachOBigEndianMagic() const
{
    return ( (this->magic == MH_CIGAM) || (this->magic == MH_CIGAM_64) );
}


void MachOFile::forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const
{
    bool stop = false;
    const load_command* startCmds = nullptr;
    if ( this->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)this + sizeof(mach_header_64));
    else if ( this->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)this + sizeof(mach_header));
    else if ( hasMachOBigEndianMagic() )
        return;  // can't process big endian mach-o
    else {
        const uint32_t* h = (uint32_t*)this;
        diag.error("file does not start with MH_MAGIC[_64]: 0x%08X 0x%08X", h[0], h [1]);
        return;  // not a mach-o file
    }
    if ( this->filetype > 12 ) {
        diag.error("unknown mach-o filetype (%u)", this->filetype);
        return;
    }
    const load_command* const cmdsEnd  = (load_command*)((char*)startCmds + this->sizeofcmds);
    const load_command* const cmdsLast = (load_command*)((char*)startCmds + this->sizeofcmds - sizeof(load_command));
    const load_command*       cmd      = startCmds;
    for (uint32_t i = 0; i < this->ncmds; ++i) {
        if ( cmd > cmdsLast ) {
            diag.error("malformed load command #%u of %u at %p with mh=%p, extends past sizeofcmds", i, this->ncmds, cmd, this);
            return;
        }
        uint32_t cmdsize = cmd->cmdsize;
        if ( cmdsize < 8 ) {
            diag.error("malformed load command #%u of %u at %p with mh=%p, size (0x%X) too small", i, this->ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        if ( (cmdsize % 4) != 0 ) {
            // FIXME: on 64-bit mach-o, should be 8-byte aligned, (might reveal bin-compat issues)
            diag.error("malformed load command #%u of %u at %p with mh=%p, size (0x%X) not multiple of 4", i, this->ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        const load_command* nextCmd = (load_command*)((char *)cmd + cmdsize);
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            diag.error("malformed load command #%u of %u at %p with mh=%p, size (0x%X) is too large, load commands end at %p", i, this->ncmds, cmd, this, cmd->cmdsize, cmdsEnd);
            return;
        }
        callback(cmd, stop);
        if ( stop )
            return;
        cmd = nextCmd;
    }
}

void MachOFile::removeLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& remove, bool& stop))
{
    bool stop = false;
    const load_command* startCmds = nullptr;
    if ( this->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)this + sizeof(mach_header_64));
    else if ( this->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)this + sizeof(mach_header));
    else if ( hasMachOBigEndianMagic() )
        return;  // can't process big endian mach-o
    else {
        const uint32_t* h = (uint32_t*)this;
        diag.error("file does not start with MH_MAGIC[_64]: 0x%08X 0x%08X", h[0], h [1]);
        return;  // not a mach-o file
    }
    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + this->sizeofcmds);
    auto cmd = (load_command*)startCmds;
    const uint32_t origNcmds = this->ncmds;
    unsigned bytesRemaining = this->sizeofcmds;
    for (uint32_t i = 0; i < origNcmds; ++i) {
        bool remove = false;
        auto nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
        if ( cmd->cmdsize < 8 ) {
            diag.error("malformed load command #%d of %d at %p with mh=%p, size (0x%X) too small", i, this->ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            diag.error("malformed load command #%d of %d at %p with mh=%p, size (0x%X) is too large, load commands end at %p", i, this->ncmds, cmd, this, cmd->cmdsize, cmdsEnd);
            return;
        }
        callback(cmd, remove, stop);
        if ( remove ) {
            this->sizeofcmds -= cmd->cmdsize;
            ::memmove((void*)cmd, (void*)nextCmd, bytesRemaining);
            this->ncmds--;
        } else {
            bytesRemaining -= cmd->cmdsize;
            cmd = nextCmd;
        }
        if ( stop )
            break;
    }
    if ( cmd )
     ::bzero(cmd, bytesRemaining);
}


bool MachOFile::hasObjC() const
{
    __block bool result = false;
    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.sectionName == "__objc_imageinfo") && info.segmentName.starts_with("__DATA") ) {
            result = true;
            stop = true;
        }
        if ( (this->cputype == CPU_TYPE_I386) && (info.sectionName == "__image_info") && (info.segmentName == "__OBJC") ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOFile::hasConstObjCSection() const
{
    return hasSection("__DATA_CONST", "__objc_selrefs")
        || hasSection("__DATA_CONST", "__objc_classrefs")
        || hasSection("__DATA_CONST", "__objc_protorefs")
        || hasSection("__DATA_CONST", "__objc_superrefs");
}

bool MachOFile::hasSection(const char* segName, const char* sectName) const
{
    __block bool result = false;
    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.segmentName == segName) && (info.sectionName == sectName) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

void MachOFile::forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const
{
    Diagnostics       diag;
    __block unsigned  count   = 0;
    __block bool      stopped = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                const char* loadPath = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                callback(loadPath, (cmd->cmd == LC_LOAD_WEAK_DYLIB), (cmd->cmd == LC_REEXPORT_DYLIB), (cmd->cmd == LC_LOAD_UPWARD_DYLIB),
                                    dylibCmd->dylib.compatibility_version, dylibCmd->dylib.current_version, stop);
                ++count;
                if ( stop )
                    stopped = true;
            }
            break;
        }
    });
    (void)count;
    (void)stopped;
#if !BUILDING_SHARED_CACHE_UTIL && !BUILDING_DYLDINFO && !BUILDING_UNIT_TESTS
    // everything must link with something
    if ( (count == 0) && !stopped ) {
        // The dylibs that make up libSystem can link with nothing
        // except for dylibs in libSystem.dylib which are ok to link with nothing (they are on bottom)
        const Header* hdr = (const Header*)this;
#if TARGET_OS_EXCLAVEKIT
        if ( !this->isDylib() || (strncmp(hdr->installName(), "/System/ExclaveKit/usr/lib/system/", 34) != 0) )
            callback("/System/ExclaveKit/usr/lib/libSystem.dylib", false, false, false, 0x00010000, 0x00010000, stopped);
#else
        if ( hdr->builtForPlatform(Platform::driverKit, true) ) {
            if ( !this->isDylib() || (strncmp(hdr->installName(), "/System/DriverKit/usr/lib/system/", 33) != 0) )
                callback("/System/DriverKit/usr/lib/libSystem.B.dylib", false, false, false, 0x00010000, 0x00010000, stopped);
        }
        else if (   hdr->builtForPlatform(Platform::macOS_exclaveKit, true)
                 || hdr->builtForPlatform(Platform::iOS_exclaveKit, true)
                 || hdr->builtForPlatform(Platform::tvOS_exclaveKit, true)
                 || hdr->builtForPlatform(Platform::watchOS_exclaveKit, true)
                 || hdr->builtForPlatform(Platform::visionOS_exclaveKit, true) ) {
            // do nothing for ExclaveKit dylibs
            // FIXME: only allow this behavior on internal builds
        }
        else {
            if ( !this->isDylib() || (strncmp(hdr->installName(), "/usr/lib/system/", 16) != 0) )
                callback("/usr/lib/libSystem.B.dylib", false, false, false, 0x00010000, 0x00010000, stopped);
        }
#endif // TARGET_OS_EXCLAVEKIT
    }
#endif // !BUILDING_SHARED_CACHE_UTIL && !BUILDING_DYLDINFO && !BUILDING_UNIT_TESTS
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

uint32_t MachOFile::entryAddrRegisterIndexForThreadCmd() const
{
    switch ( this->cputype ) {
        case CPU_TYPE_I386:
            return 10; // i386_thread_state_t.eip
        case CPU_TYPE_X86_64:
            return 16; // x86_thread_state64_t.rip
        case CPU_TYPE_ARM:
            return 15; // arm_thread_state_t.pc
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM64_32:
            return 32; // arm_thread_state64_t.__pc
    }
    return ~0U;
}

bool MachOFile::use64BitEntryRegs() const
{
    return is64() || isArch("arm64_32");
}

uint64_t MachOFile::entryAddrFromThreadCmd(const thread_command* cmd) const
{
    assert(cmd->cmd == LC_UNIXTHREAD);
    const uint32_t* regs32 = (uint32_t*)(((char*)cmd) + 16);
    const uint64_t* regs64 = (uint64_t*)(((char*)cmd) + 16);

    uint32_t index = entryAddrRegisterIndexForThreadCmd();
    if (index == ~0U)
        return 0;

    return use64BitEntryRegs() ? regs64[index] : regs32[index];
}

void MachOFile::forEachSection(void (^callback)(const Header::SectionInfo&, bool& stop)) const
{
    ((const Header*)this)->forEachSection(callback);
}

void MachOFile::forEachSection(void (^callback)(const Header::SegmentInfo&, const Header::SectionInfo&, bool& stop)) const
{
    ((const Header*)this)->forEachSection(callback);
}

bool MachOFile::hasWeakDefs() const
{
    return (this->flags & MH_WEAK_DEFINES);
}

bool MachOFile::usesWeakDefs() const
{
    return (this->flags & MH_BINDS_TO_WEAK);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_UNIT_TESTS || BUILDING_DYLD_SYMBOLS_CACHE
static bool endsWith(const char* str, const char* suffix)
{
    size_t strLen    = strlen(str);
    size_t suffixLen = strlen(suffix);
    if ( strLen < suffixLen )
        return false;
    return (strcmp(&str[strLen-suffixLen], suffix) == 0);
}

static bool startsWith(const char* buffer, const char* valueToFind) {
    return strncmp(buffer, valueToFind, strlen(valueToFind)) == 0;
}

static bool platformExcludesSharedCache_macOS(const char* installName) {
    // Note: This function basically matches dontCache() from update dyld shared cache

    if ( startsWith(installName, "/usr/lib/system/introspection/") )
        return true;
    if ( startsWith(installName, "/System/Library/QuickTime/") )
        return true;
    if ( startsWith(installName, "/System/Library/Tcl/") )
        return true;
    if ( startsWith(installName, "/System/Library/Perl/") )
        return true;
    if ( startsWith(installName, "/System/Library/MonitorPanels/") )
        return true;
    if ( startsWith(installName, "/System/Library/Accessibility/") )
        return true;
    if ( startsWith(installName, "/usr/local/") )
        return true;
    if ( startsWith(installName, "/usr/lib/pam/") )
        return true;
    // We no longer support ROSP, so skip all paths which start with the special prefix
    if ( startsWith(installName, "/System/Library/Templates/Data/") )
        return true;

    // anything inside a .app bundle is specific to app, so should not be in shared cache
    if ( strstr(installName, ".app/") != NULL )
        return true;

    // Depends on UHASHelloExtensionPoint-macOS which is not always cache eligible
    if ( !strcmp(installName, "/System/Library/PrivateFrameworks/HelloWorldMacHelper.framework/Versions/A/HelloWorldMacHelper") )
        return true;

    return false;
}

static bool platformExcludesSharedCache_iOS(const char* installName) {
    if ( strcmp(installName, "/System/Library/Caches/com.apple.xpc/sdk.dylib") == 0 )
        return true;
    if ( strcmp(installName, "/System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib") == 0 )
        return true;
    return false;
}

// Returns true if the current platform requires that this install name be excluded from the shared cache
// Note that this overrides any exclusion from anywhere else.
static bool platformExcludesSharedCache(Platform platform, const char* installName) {
    if ( (platform == Platform::macOS) || (platform == Platform::macCatalyst) || (platform == Platform::zippered) )
        return platformExcludesSharedCache_macOS(installName);
    // Everything else is based on iOS so just use that value
    return platformExcludesSharedCache_iOS(installName);
}

#if !BUILDING_DYLD

bool MachOFile::addendsExceedPatchTableLimit(Diagnostics& diag, mach_o::Fixups fixups) const
{
    // rdar://122906481 (Shared cache builder - explicitly model dylibs without a need for a patch table)
    if ( strcmp(((const Header*)this)->installName(), "/usr/lib/libswiftPrespecialized.dylib") == 0 )
        return false;

    const bool     is64bit = is64();
    const uint64_t tooLargeRegularAddend = 1 << 23;
    const uint64_t tooLargeAuthAddend = 1 << 5;
    __block bool addendTooLarge = false;
    if ( this->hasChainedFixups() ) {

        // with chained fixups, addends can be in the import table or embedded in a bind pointer
        __block std::vector<uint64_t> targetAddends;
        fixups.forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
            if ( is64bit )
                addend &= 0x00FFFFFFFFFFFFFF; // ignore TBI
            targetAddends.push_back(addend);
        });
        // check each pointer for embedded addend
        fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
            fixups.forEachFixupInAllChains(diag, starts, false, ^(mach_o::ChainedFixupPointerOnDisk* fixupLoc, uint64_t fixupSegmentOffset, const dyld_chained_starts_in_segment* segInfo, bool& stop) {
                switch (segInfo->pointer_format) {
                    case DYLD_CHAINED_PTR_ARM64E:
                    case DYLD_CHAINED_PTR_ARM64E_USERLAND:
                        if ( fixupLoc->arm64e.bind.bind ) {
                            uint64_t ordinal = fixupLoc->arm64e.bind.ordinal;
                            uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                            if ( fixupLoc->arm64e.bind.auth ) {
                                if ( addend >= tooLargeAuthAddend ) {
                                    addendTooLarge = true;
                                    stop = true;
                                }
                            } else {
                                addend += fixupLoc->arm64e.signExtendedAddend();
                                if ( addend >= tooLargeRegularAddend ) {
                                    addendTooLarge = true;
                                    stop = true;
                                }
                            }
                        }
                        break;
                    case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                        if ( fixupLoc->arm64e.bind24.bind ) {
                            uint64_t ordinal = fixupLoc->arm64e.bind24.ordinal;
                            uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                            if ( fixupLoc->arm64e.bind24.auth ) {
                                if ( addend >= tooLargeAuthAddend ) {
                                    addendTooLarge = true;
                                    stop = true;
                                }
                            } else {
                                addend += fixupLoc->arm64e.signExtendedAddend();
                                if ( addend >= tooLargeRegularAddend ) {
                                    addendTooLarge = true;
                                    stop = true;
                                }
                            }
                        }
                        break;
                    case DYLD_CHAINED_PTR_64:
                    case DYLD_CHAINED_PTR_64_OFFSET: {
                        if ( fixupLoc->generic64.rebase.bind ) {
                            uint64_t ordinal = fixupLoc->generic64.bind.ordinal;
                            uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                            addend += fixupLoc->generic64.bind.addend;
                            if ( addend >= tooLargeRegularAddend ) {
                                addendTooLarge = true;
                                stop = true;
                            }
                        }
                        break;
                    }
                    case DYLD_CHAINED_PTR_32:
                        if ( fixupLoc->generic32.bind.bind ) {
                            uint64_t ordinal = fixupLoc->generic32.bind.ordinal;
                            uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                            addend += fixupLoc->generic32.bind.addend;
                            if ( addend >= tooLargeRegularAddend ) {
                                addendTooLarge = true;
                                stop = true;
                            }
                        }
                        break;
                }
            });
        });
    }
    else {
        // scan bind opcodes for large addend
        auto handler = ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
            uint64_t addend = info.addend;
            if ( is64bit )
                addend &= 0x00FFFFFFFFFFFFFF; // ignore TBI
            if ( addend >= tooLargeRegularAddend ) {
                addendTooLarge = true;
                stop = true;
            }
        };
        fixups.forEachBindTarget_Opcodes(diag, true, handler, handler);
    }

    return addendTooLarge;
}

bool MachOFile::canBePlacedInDyldCache(const char* path, bool checkObjC, void (^failureReason)(const char* format, ...)) const
{
    if ( !Header::isSharedCacheEligiblePath(path) ) {
        // Dont spam the user with an error about paths when we know these are never eligible.
        return false;
    }

    // only dylibs can go in cache
    if ( !this->isDylib() && !this->isDyld() ) {
        failureReason("Not MH_DYLIB");
        return false; // cannot continue, installName() will assert() if not a dylib
    }


    const char* dylibName = ((const Header*)this)->installName();
    if ( dylibName[0] != '/' ) {
        failureReason("install name not an absolute path");
        // Don't continue as we don't want to spam the log with errors we don't need.
        return false;
    }
    else if ( strcmp(dylibName, path) != 0 ) {
        failureReason("install path does not match install name");
        return false;
    }
    else if ( strstr(dylibName, "//") != 0 ) {
        failureReason("install name should not include //");
        return false;
    }
    else if ( strstr(dylibName, "./") != 0 ) {
        failureReason("install name should not include ./");
        return false;
    }

    mach_o::PlatformAndVersions pvs = ((const Header*)this)->platformAndVersions();
    bool platformExcludedFile = platformExcludesSharedCache(pvs.platform, dylibName);
    
    if ( platformExcludedFile ) {
        failureReason("install name is not shared cache eligible on platform");
        return false;
    }

    // flat namespace files cannot go in cache
    if ( (this->flags & MH_TWOLEVEL) == 0 ) {
        failureReason("Not built with two level namespaces");
        return false;
    }

    // don't put debug variants into dyld cache
    if ( endsWith(path, "_profile.dylib") || endsWith(path, "_debug.dylib") || endsWith(path, "_asan.dylib")
        || endsWith(path, "_profile") || endsWith(path, "_debug") || endsWith(path, "_asan")
        || endsWith(path, "/CoreADI") ) {
        failureReason("Variant image");
        return false;
    }

    // dylib must have extra info for moving DATA and TEXT segments apart
    __block bool hasExtraInfo = false;
    __block bool hasSplitSegMarker = false;
    __block bool hasDyldInfo = false;
    __block bool hasExportTrie = false;
    __block Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO ) {
            const linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            if ( sigCmd->datasize == 0 )
                hasSplitSegMarker = true;
            else
                hasExtraInfo = true;
        }
        if ( cmd->cmd == LC_DYLD_INFO_ONLY )
            hasDyldInfo = true;
        if ( cmd->cmd == LC_DYLD_EXPORTS_TRIE )
            hasExportTrie = true;
    });
    if ( !hasExtraInfo ) {
        std::string_view ignorePaths[] = {
            "/usr/lib/libobjc-trampolines.dylib",
            "/usr/lib/libffi-trampolines.dylib"
        };
        for ( std::string_view ignorePath : ignorePaths ) {
            if ( ignorePath == path )
                return false;
        }
        if ( hasSplitSegMarker )
            failureReason("Dylib explicitly linked with '-not_for_dyld_shared_cache'");
        else
            failureReason("Missing split seg info");
        return false;
    }
    if ( !hasDyldInfo && !hasExportTrie ) {
        failureReason("Old binary, missing dyld info or export trie");
        return false;
    }

    // dylib can only depend on other dylibs in the shared cache
    __block const char* badDep = nullptr;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        // Skip weak links.  They are allowed to be missing
        if ( isWeak )
            return;
        if ( !Header::isSharedCacheEligiblePath(loadPath) ) {
            badDep = loadPath;
            stop = true;
        }
    });
    if ( badDep != nullptr ) {
        failureReason("Depends on dylibs ineligible for dyld cache '%s'.  (cache dylibs must start /usr/lib or /System/Library or similar)",
                      badDep);
        return false;
    }

    // dylibs with interposing info cannot be in cache
    if ( ((const Header*)this)->hasInterposingTuples() ) {
        failureReason("Has interposing tuples");
        return false;
    }

    // Temporarily kick out swift binaries out of dyld cache on watchOS simulators as they have missing split seg
    if ( (this->cputype == CPU_TYPE_I386) && ((const Header*)this)->builtForPlatform(Platform::watchOS_simulator) ) {
        if ( strncmp(dylibName, "/usr/lib/swift/", 15) == 0 ) {
            failureReason("i386 swift binary");
            return false;
        }
    }

    // These used to be in MachOAnalyzer
    __block bool passedLinkeditChecks = false;
    this->withFileLayout(diag, ^(const mach_o::Layout &layout) {

        mach_o::SplitSeg splitSeg(layout);
        mach_o::Fixups fixups(layout);

        // arm64e requires split seg v2 as the split seg code can't handle chained fixups for split seg v1
        if ( isArch("arm64e") ) {
            if ( !splitSeg.isV2() ) {
                failureReason("chained fixups requires split seg v2");
                return;
            }
        }

        // evict swift dylibs with split seg v1 info
        if ( layout.isSwiftLibrary() && splitSeg.isV1() )
            return;

        // arm64e requires signed class ROs
        if ( isArch("arm64e") ) {
            if ( std::optional<uint32_t> flags = layout.getObjcInfoFlags(); flags.has_value() ) {
                if ( (flags.value() & mach_o::ObjCImageInfo::OBJC_IMAGE_SIGNED_CLASS_RO) == 0 ) {
                    failureReason("arm64e binaries must have signed Objective-C class_ro_t pointers");
                    return;
                }
            }
        }

        if ( splitSeg.isV1() ) {
            // Split seg v1 can only support 1 __DATA, and no other writable segments
            __block bool foundBadSegment = false;
            ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
                if ( info.initProt == (VM_PROT_READ | VM_PROT_WRITE) ) {
                    if ( info.segmentName == "__DATA" )
                        return;

                    failureReason("RW segments other than __DATA requires split seg v2");
                    foundBadSegment = true;
                    stop = true;
                }
            });

            if ( foundBadSegment )
                return;
        }

        // <rdar://problem/57769033> dyld_cache_patchable_location only supports addend in range 0..31
        // rdar://96164956 (dyld needs to support arbitrary addends in cache patch table)
        bool addendTooLarge = addendsExceedPatchTableLimit(diag, fixups);
        if ( addendTooLarge ) {
            failureReason("bind addend too large");
            return;
        }

        if ( (isArch("x86_64") || isArch("x86_64h")) ) {
            __block bool rebasesOk = true;
            uint64_t startVMAddr = ((const Header*)this)->preferredLoadAddress();
            uint64_t endVMAddr = startVMAddr + mappedSize();
            fixups.forEachRebase(diag, ^(uint64_t runtimeOffset, uint64_t rebasedValue, bool &stop) {
                // We allow TBI for x86_64 dylibs, but then require that the remainder of the offset
                // is a 32-bit offset from the mach-header.
                rebasedValue &= 0x00FFFFFFFFFFFFFFULL;
                if ( (rebasedValue < startVMAddr) || (rebasedValue >= endVMAddr) ) {
                    failureReason("rebase value out of range of dylib");
                    rebasesOk = false;
                    stop = true;
                    return;
                }

                // Also error if the rebase location is anything other than 4/8 byte aligned
                if ( (runtimeOffset & 0x3) != 0 ) {
                    failureReason("rebase value is not 4-byte aligned");
                    rebasesOk = false;
                    stop = true;
                    return;
                }

                // Error if the fixup will cross a page
                if ( (runtimeOffset & 0xFFF) == 0xFFC ) {
                    failureReason("rebase value crosses page boundary");
                    rebasesOk = false;
                    stop = true;
                    return;
                }
            });

            if ( !rebasesOk )
                return;

            if ( this->hasChainedFixups() ) {
                fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                    fixups.forEachFixupInAllChains(diag, starts, false, ^(mach_o::ChainedFixupPointerOnDisk* fixupLoc, uint64_t fixupSegmentOffset, const dyld_chained_starts_in_segment* segInfo, bool& stop) {
                        if ( (fixupSegmentOffset & 0xFFF) == 0xFFC ) {
                            failureReason("chained fixup crosses page boundary");
                            rebasesOk = false;
                            stop = true;
                            return;
                        }
                    });
                });
            }

            if ( !rebasesOk )
                return;
        }

        // Check that shared cache dylibs don't use undefined lookup
        {
            __block bool bindsOk = true;

            auto checkBind = ^(int libOrdinal, bool& stop) {
                if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
                    failureReason("has dynamic_lookup binds");
                    bindsOk = false;
                    stop = true;
                }
            };

            if (hasChainedFixups()) {
                fixups.forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
                    checkBind(libOrdinal, stop);
                });
            } else {
                auto handler = ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                    checkBind(info.libOrdinal, stop);
                };
                fixups.forEachBindTarget_Opcodes(diag, true, handler, handler);
            }

            if ( !bindsOk )
                return;
        }

        passedLinkeditChecks = true;
    });

    if ( !passedLinkeditChecks )
        return false;

    // Check there are no pointer based objc method lists in CONST segments
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    if ( checkObjC ) {
        typedef std::pair<VMAddress, VMAddress> Range;
        __block std::vector<Range> constRanges;
        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
            if ( info.vmsize == 0 )
                return;
            if ( info.segmentName == "__DATA_CONST" || info.segmentName == "__AUTH_CONST" )
                constRanges.push_back({ VMAddress(info.vmaddr), VMAddress(info.vmaddr + info.vmsize) });
        });

        if ( !constRanges.empty() ) {
            __block objc_visitor::Visitor objcVisitor = this->makeObjCVisitor(diag);
            if ( diag.hasError() )
                return false;

            // Returns true if the method list is bad, ie, a pointer based method list in _CONST segment
            auto isConstPointerBasedMethodList = ^(const objc_visitor::MethodList& methodList) {
                if ( (methodList.numMethods() == 0) || methodList.usesRelativeOffsets() )
                    return false;

                VMAddress methodListVMAddr = methodList.getVMAddress().value();
                for ( const Range& range : constRanges ) {
                    if ( (methodListVMAddr >= range.first) && (methodListVMAddr < range.second) )
                        return true;
                }

                return false;
            };

            __block bool hasPointerMethodList = false;
            objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
                if ( isConstPointerBasedMethodList(objcClass.getBaseMethods(objcVisitor)) ) {
                    failureReason("has pointer based objc class method list in _CONST segment");
                    hasPointerMethodList = true;
                    stopClass = true;
                }
            });
            if ( hasPointerMethodList )
                return false;

            objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool& stopCategory) {
                if ( isConstPointerBasedMethodList(objcCategory.getInstanceMethods(objcVisitor)) ) {
                    failureReason("has pointer based objc category instance method list in _CONST segment");
                    hasPointerMethodList = true;
                    stopCategory = true;
                }
                if ( isConstPointerBasedMethodList(objcCategory.getClassMethods(objcVisitor)) ) {
                    failureReason("has pointer based objc category class method list in _CONST segment");
                    hasPointerMethodList = true;
                    stopCategory = true;
                }
            });
            if ( hasPointerMethodList )
                return false;
        }
    }
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

    return true;
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
objc_visitor::Visitor MachOFile::makeObjCVisitor(Diagnostics& diag) const
{
    VMAddress dylibBaseAddress(((const Header*)this)->preferredLoadAddress());

    __block std::vector<metadata_visitor::Segment> segments;
    __block std::vector<uint64_t> bindTargets;
    this->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        for ( uint32_t segIndex = 0; segIndex != layout.segments.size(); ++segIndex ) {
            const auto& layoutSegment = layout.segments[segIndex];
            metadata_visitor::Segment segment {
                .startVMAddr = VMAddress(layoutSegment.vmAddr),
                .endVMAddr = VMAddress(layoutSegment.vmAddr + layoutSegment.vmSize),
                .bufferStart = (uint8_t*)layoutSegment.buffer,
                .onDiskDylibChainedPointerFormat = 0,
                .segIndex = segIndex
            };
            segments.push_back(std::move(segment));
        }

        // Add chained fixup info to each segment, if we have it
        if ( this->hasChainedFixups() ) {
            mach_o::Fixups fixups(layout);
            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                mach_o::Fixups::forEachFixupChainSegment(diag, starts,
                                                         ^(const dyld_chained_starts_in_segment *segInfo, uint32_t segIndex, bool &stop) {
                    segments[segIndex].onDiskDylibChainedPointerFormat = segInfo->pointer_format;
                });
            });
        }

        // ObjC patching needs the bind targets for interposable references to the classes
        // build targets table
        if ( this->hasChainedFixupsLoadCommand() ) {
            mach_o::Fixups fixups(layout);
            fixups.forEachBindTarget_ChainedFixups(diag, ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                if ( info.libOrdinal != BIND_SPECIAL_DYLIB_SELF ) {
                    bindTargets.push_back(0);
                    return;
                }

                mach_o::Layout::FoundSymbol foundInfo;
                if ( !layout.findExportedSymbol(diag, info.symbolName, info.weakImport, foundInfo) ) {
                    bindTargets.push_back(0);
                    return;
                }

                // We only support header offsets in this dylib, as we are looking for self binds
                // which are likely only to classes
                if ( (foundInfo.kind != mach_o::Layout::FoundSymbol::Kind::headerOffset)
                    || (foundInfo.foundInDylib.value() != this) ) {
                    bindTargets.push_back(0);
                    return;
                }

                uint64_t vmAddr = layout.textUnslidVMAddr() + foundInfo.value;
                bindTargets.push_back(vmAddr);
            });
        }
    });

    std::optional<VMAddress> selectorStringsBaseAddress;
    objc_visitor::Visitor objcVisitor(dylibBaseAddress, this,
                                      std::move(segments), selectorStringsBaseAddress,
                                      std::move(bindTargets));

    return objcVisitor;
}
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

#endif // !BUILDING_DYLD


// Returns true if the executable path is eligible for a PrebuiltLoader on the given platform.
bool MachOFile::canHavePrebuiltExecutableLoader(Platform platform, const std::string_view& path,
                                                void (^failureReason)(const char*)) const
{
    // For now we can't build prebuilt loaders for the simulator
    if ( platform.isSimulator() ) {
        // Don't spam with tons of messages about executables
        return false;
    }

    if ( (platform == Platform::macOS) || (platform == Platform::macCatalyst) ) {
        // We no longer support ROSP, so skip all paths which start with the special prefix
        if ( path.starts_with("/System/Library/Templates/Data/") ) {
            // Dont spam the user with an error about paths when we know these are never eligible.
            return false;
        }

        static const char* sAllowedPrefixes[] = {
            "/bin/",
            "/sbin/",
            "/usr/",
            "/System/",
            "/Library/Apple/System/",
            "/Library/Apple/usr/",
            "/System/Applications/Safari.app/",
            "/Library/CoreMediaIO/Plug-Ins/DAL/" // temp until plugins moved or closured working
        };

        bool inSearchDir = false;
        for ( const char* searchDir : sAllowedPrefixes ) {
            if ( path.starts_with(searchDir) ) {
                inSearchDir = true;
                break;
            }
        }

        if ( !inSearchDir ) {
            failureReason("path not eligible");
            return false;
        }
    }

    if ( !this->hasLoadCommand(LC_CODE_SIGNATURE) ) {
        failureReason("missing code signature");
        return false;
    }

    return true;
}
#endif

#if BUILDING_APP_CACHE_UTIL
bool MachOFile::canBePlacedInKernelCollection(const char* path, void (^failureReason)(const char*)) const
{
    // only dylibs and the kernel itself can go in cache
    if ( this->filetype == MH_EXECUTE ) {
        // xnu
    } else if ( this->isKextBundle() ) {
        // kext's
    } else {
        failureReason("Not MH_KEXT_BUNDLE");
        return false;
    }

    if ( this->filetype == MH_EXECUTE ) {
        // xnu

        // two-level namespace binaries cannot go in cache
        if ( (this->flags & MH_TWOLEVEL) != 0 ) {
            failureReason("Built with two level namespaces");
            return false;
        }

        // xnu kernel cannot have a page zero
        __block bool foundPageZero = false;
        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo &segmentInfo, bool &stop) {
            if ( segmentInfo.segmentName == "__PAGEZERO" ) {
                foundPageZero = true;
                stop = true;
            }
        });
        if (foundPageZero) {
            failureReason("Has __PAGEZERO");
            return false;
        }

        // xnu must have an LC_UNIXTHREAD to point to the entry point
        __block bool foundMainLC = false;
        __block bool foundUnixThreadLC = false;
        Diagnostics diag;
        forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
            if ( cmd->cmd == LC_MAIN ) {
                foundMainLC = true;
                stop = true;
            }
            else if ( cmd->cmd == LC_UNIXTHREAD ) {
                foundUnixThreadLC = true;
            }
        });
        if (foundMainLC) {
            failureReason("Found LC_MAIN");
            return false;
        }
        if (!foundUnixThreadLC) {
            failureReason("Expected LC_UNIXTHREAD");
            return false;
        }

        if (diag.hasError()) {
            failureReason("Error parsing load commands");
            return false;
        }

        // The kernel should be a static executable, not a dynamic one
        if ( !isStaticExecutable() ) {
            failureReason("Expected static executable");
            return false;
        }

        // The kernel must be built with -pie
        if ( !isPIE() ) {
            failureReason("Expected pie");
            return false;
        }
    }

    if ( isArch("arm64e") && isKextBundle() && !hasChainedFixups() ) {
        failureReason("Missing fixup information");
        return false;
    }

    // dylibs with interposing info cannot be in cache
    if ( ((const Header*)this)->hasInterposingTuples() ) {
        failureReason("Has interposing tuples");
        return false;
    }

    // Only x86_64 is allowed to have RWX segments
    if ( !isArch("x86_64") && !isArch("x86_64h") ) {
        __block bool foundBadSegment = false;
        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
            if ( (info.initProt & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == (VM_PROT_WRITE | VM_PROT_EXECUTE) ) {
                failureReason("Segments are not allowed to be both writable and executable");
                foundBadSegment = true;
                stop = true;
            }
        });
        if ( foundBadSegment )
            return false;
    }

    return true;
}
#endif // BUILDING_APP_CACHE_UTIL

#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
bool MachOFile::usesClassicRelocationsInKernelCollection() const {
    // The xnu x86_64 static executable needs to do the i386->x86_64 transition
    // so will be emitted with classic relocations
    if ( isArch("x86_64") || isArch("x86_64h") ) {
        return isStaticExecutable() || isFileSet();
    }
    return false;
}
#endif // BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO

bool MachOFile::hasLoadCommand(uint32_t cmdNum) const
{
    __block bool hasLC = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == cmdNum ) {
            hasLC = true;
            stop = true;
        }
    });
    return hasLC;
}

bool MachOFile::hasChainedFixups() const
{
#if SUPPORT_ARCH_arm64e
    // arm64e always uses chained fixups
    if ( (this->cputype == CPU_TYPE_ARM64) && (this->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E) ) {
        // Not all binaries have fixups at all so check for the load commands
        return hasLoadCommand(LC_DYLD_INFO_ONLY) || hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
    }
#endif
    return hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
}

bool MachOFile::hasChainedFixupsLoadCommand() const
{
    return hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
}

bool MachOFile::hasOpcodeFixups() const
{
    return hasLoadCommand(LC_DYLD_INFO_ONLY) || hasLoadCommand(LC_DYLD_INFO) ;
}

uint16_t MachOFile::chainedPointerFormat(const dyld_chained_fixups_header* header)
{
    const dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)((uint8_t*)header + header->starts_offset);
    for (uint32_t i=0; i < startsInfo->seg_count; ++i) {
        uint32_t segInfoOffset = startsInfo->seg_info_offset[i];
        // 0 offset means this segment has no fixups
        if ( segInfoOffset == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + segInfoOffset);
        if ( segInfo->page_count != 0 )
            return segInfo->pointer_format;
    }
    return 0;  // no chains (perhaps no __DATA segment)
}

// find dyld_chained_starts_in_image* in image
// if old arm64e binary, synthesize dyld_chained_starts_in_image*
void MachOFile::withChainStarts(Diagnostics& diag, const dyld_chained_fixups_header* chainHeader, void (^callback)(const dyld_chained_starts_in_image*))
{
    if ( chainHeader == nullptr ) {
        diag.error("Must pass in a chain header");
        return;
    }
    // we have a pre-computed offset into LINKEDIT for dyld_chained_starts_in_image
    callback((dyld_chained_starts_in_image*)((uint8_t*)chainHeader + chainHeader->starts_offset));
}

void MachOFile::forEachFixupChainSegment(Diagnostics& diag, const dyld_chained_starts_in_image* starts,
                                           void (^handler)(const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool& stop))
{
    bool stopped = false;
    for (uint32_t segIndex=0; segIndex < starts->seg_count && !stopped; ++segIndex) {
        if ( starts->seg_info_offset[segIndex] == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
        handler(segInfo, segIndex, stopped);
    }
}


bool MachOFile::walkChain(Diagnostics& diag, ChainedFixupPointerOnDisk* chain, uint16_t pointer_format, bool notifyNonPointers, uint32_t max_valid_pointer,
                          void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop))
{
    const unsigned stride = ChainedFixupPointerOnDisk::strideSize(pointer_format);
    bool  stop = false;
    bool  chainEnd = false;
    while (!stop && !chainEnd) {
        // copy chain content, in case handler modifies location to final value
        ChainedFixupPointerOnDisk chainContent = *chain;
        handler(chain, stop);

        if ( !stop ) {
            switch (pointer_format) {
                case DYLD_CHAINED_PTR_ARM64E:
                case DYLD_CHAINED_PTR_ARM64E_KERNEL:
                case DYLD_CHAINED_PTR_ARM64E_USERLAND:
                case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
                    if ( chainContent.arm64e.rebase.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.arm64e.rebase.next*stride);
                    break;
                case DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE:
                    if ( chainContent.cache64e.regular.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.cache64e.regular.next*stride);
                    break;
                case DYLD_CHAINED_PTR_64:
                case DYLD_CHAINED_PTR_64_OFFSET:
                    if ( chainContent.generic64.rebase.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.generic64.rebase.next*4);
                    break;
                case DYLD_CHAINED_PTR_32:
                    if ( chainContent.generic32.rebase.next == 0 )
                        chainEnd = true;
                    else {
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.generic32.rebase.next*4);
                        if ( !notifyNonPointers ) {
                            while ( (chain->generic32.rebase.bind == 0) && (chain->generic32.rebase.target > max_valid_pointer) ) {
                                // not a real pointer, but a non-pointer co-opted into chain
                                chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chain->generic32.rebase.next*4);
                            }
                        }
                    }
                    break;
                case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
                case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
                    if ( chainContent.kernel64.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.kernel64.next*stride);
                    break;
                case DYLD_CHAINED_PTR_32_FIRMWARE:
                    if ( chainContent.firmware32.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.firmware32.next*4);
                    break;
                default:
                    diag.error("unknown pointer format 0x%04X", pointer_format);
                    stop = true;
            }
        }
    }
    return stop;
}

void MachOFile::forEachFixupInSegmentChains(Diagnostics& diag, const dyld_chained_starts_in_segment* segInfo,
                                            bool notifyNonPointers, uint8_t* segmentContent,
                                            void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop))
{
    bool stopped = false;
    for (uint32_t pageIndex=0; pageIndex < segInfo->page_count && !stopped; ++pageIndex) {
        uint16_t offsetInPage = segInfo->page_start[pageIndex];
        if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
            continue;
        if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
            // 32-bit chains which may need multiple starts per page
            uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
            bool chainEnd = false;
            while (!stopped && !chainEnd) {
                chainEnd = (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                offsetInPage = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                uint8_t* pageContentStart = segmentContent + (pageIndex * segInfo->page_size);
                ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);
                stopped = walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, handler);
                ++overflowIndex;
            }
        }
        else {
            // one chain per page
            uint8_t* pageContentStart = segmentContent + (pageIndex * segInfo->page_size);
            ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);
            stopped = walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, handler);
        }
    }
}

void MachOFile::forEachChainedFixupTarget(Diagnostics& diag, const dyld_chained_fixups_header* header,
                                          const linkedit_data_command* chainedFixups,
                                          void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop))
{
    if ( (header->imports_offset > chainedFixups->datasize) || (header->symbols_offset > chainedFixups->datasize) ) {
        diag.error("malformed import table");
        return;
    }

    bool stop    = false;

    const dyld_chained_import*          imports;
    const dyld_chained_import_addend*   importsA32;
    const dyld_chained_import_addend64* importsA64;
    const char*                         symbolsPool     = (char*)header + header->symbols_offset;
    uint32_t                            maxSymbolOffset = chainedFixups->datasize - header->symbols_offset;
    int                                 libOrdinal;
    switch (header->imports_format) {
        case DYLD_CHAINED_IMPORT:
            imports = (dyld_chained_import*)((uint8_t*)header + header->imports_offset);
            for (uint32_t i=0; i < header->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[imports[i].name_offset];
                if ( imports[i].name_offset > maxSymbolOffset ) {
                    diag.error("malformed import table, string overflow");
                    return;
                }
                uint8_t libVal = imports[i].lib_ordinal;
                if ( libVal > 0xF0 )
                    libOrdinal = (int8_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, 0, imports[i].weak_import, stop);
                if ( stop )
                    return;
            }
            break;
        case DYLD_CHAINED_IMPORT_ADDEND:
            importsA32 = (dyld_chained_import_addend*)((uint8_t*)header + header->imports_offset);
            for (uint32_t i=0; i < header->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[importsA32[i].name_offset];
                if ( importsA32[i].name_offset > maxSymbolOffset ) {
                    diag.error("malformed import table, string overflow");
                    return;
                }
                uint8_t libVal = importsA32[i].lib_ordinal;
                if ( libVal > 0xF0 )
                    libOrdinal = (int8_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, importsA32[i].addend, importsA32[i].weak_import, stop);
                if ( stop )
                    return;
            }
            break;
        case DYLD_CHAINED_IMPORT_ADDEND64:
            importsA64 = (dyld_chained_import_addend64*)((uint8_t*)header + header->imports_offset);
            for (uint32_t i=0; i < header->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[importsA64[i].name_offset];
                if ( importsA64[i].name_offset > maxSymbolOffset ) {
                    diag.error("malformed import table, string overflow");
                    return;
                }
                uint16_t libVal = importsA64[i].lib_ordinal;
                if ( libVal > 0xFFF0 )
                    libOrdinal = (int16_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, importsA64[i].addend, importsA64[i].weak_import, stop);
                if ( stop )
                    return;
            }
            break;
       default:
            diag.error("unknown imports format");
            return;
    }
}

uint64_t MachOFile::read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
{
    uint64_t result = 0;
    int         bit = 0;
    do {
        if ( p == end ) {
            diag.error("malformed uleb128");
            break;
        }
        uint64_t slice = *p & 0x7f;

        if ( bit > 63 ) {
            diag.error("uleb128 too big for uint64");
            break;
        }
        else {
            result |= (slice << bit);
            bit += 7;
        }
    }
    while (*p++ & 0x80);
    return result;
}


int64_t MachOFile::read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
{
    int64_t  result = 0;
    int      bit = 0;
    uint8_t  byte = 0;
    do {
        if ( p == end ) {
            diag.error("malformed sleb128");
            break;
        }
        byte = *p++;
        result |= (((int64_t)(byte & 0x7f)) << bit);
        bit += 7;
    } while (byte & 0x80);
    // sign extend negative numbers
    if ( ((byte & 0x40) != 0) && (bit < 64) )
        result |= (~0ULL) << bit;
    return result;
}

static void getArchNames(const GradedArchs& archs, bool isOSBinary, char buffer[256])
{
    buffer[0] = '\0';
    archs.forEachArch(isOSBinary, ^(const char* archName) {
        if ( buffer[0] != '\0' )
            strlcat(buffer, "' or '", 256);
        strlcat(buffer, archName, 256);
    });
}

const MachOFile* MachOFile::compatibleSlice(Diagnostics& diag, uint64_t& sliceOffsetOut, uint64_t& sliceLenOut, const void* fileContent, size_t contentSize, const char* path, Platform platform, bool isOSBinary, const GradedArchs& archs, bool internalInstall)
{
    const Header* mh = nullptr;
    if ( const dyld3::FatFile* ff = dyld3::FatFile::isFatFile(fileContent) ) {
        uint64_t  sliceOffset;
        uint64_t  sliceLen;
        bool      missingSlice;
        if ( ff->isFatFileWithSlice(diag, contentSize, archs, isOSBinary, sliceOffset, sliceLen, missingSlice) ) {
            mh = (const Header*)((long)fileContent + sliceOffset);
            sliceLenOut = sliceLen;
            sliceOffsetOut = sliceOffset;
        }
        else {
            BLOCK_ACCCESSIBLE_ARRAY(char, gradedArchsBuf, 256);
            getArchNames(archs, isOSBinary, gradedArchsBuf);

            char strBuf[256];
            diag.error("fat file, but missing compatible architecture (have '%s', need '%s')", ff->archNames(strBuf, contentSize), gradedArchsBuf);
            return nullptr;
        }
    }
    else {
        mh = (const Header*)fileContent;
        sliceLenOut = contentSize;
        sliceOffsetOut = 0;
    }

    std::span<const uint8_t> contents{(uint8_t*)mh, (size_t)sliceLenOut};
    if ( !Header::isMachO(contents) ) {
        diag.error("slice is not valid mach-o file");
        return nullptr;
    }

    if ( archs.grade(mh->arch().cpuType(), mh->arch().cpuSubtype(), isOSBinary) == 0 ) {
        BLOCK_ACCCESSIBLE_ARRAY(char, gradedArchsBuf, 256);
        getArchNames(archs, isOSBinary, gradedArchsBuf);
        diag.error("mach-o file, but is an incompatible architecture (have '%s', need '%s')", mh->archName(), gradedArchsBuf);
        return nullptr;
    }

    if ( !mh->loadableIntoProcess(platform, path, internalInstall) ) {
        Platform havePlatform = mh->platformAndVersions().platform;
        diag.error("mach-o file (%s), but incompatible platform (have '%s', need '%s')", path, havePlatform.name().c_str(), platform.name().c_str());
        return nullptr;
    }

    return (const MachOFile*)mh;
}

const uint8_t* MachOFile::trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol)
{
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(uint32_t, visitedNodeOffsets, 128);
    visitedNodeOffsets.push_back(0);
    const uint8_t* p = start;
    while ( p < end ) {
        uint64_t terminalSize = *p++;
        if ( terminalSize > 127 ) {
            // except for re-export-with-rename, all terminal sizes fit in one byte
            --p;
            terminalSize = read_uleb128(diag, p, end);
            if ( diag.hasError() )
                return nullptr;
        }
        if ( (*symbol == '\0') && (terminalSize != 0) ) {
            return p;
        }
        const uint8_t* children = p + terminalSize;
        if ( children > end ) {
            //diag.error("malformed trie node, terminalSize=0x%llX extends past end of trie\n", terminalSize);
            return nullptr;
        }
        uint8_t childrenRemaining = *children++;
        p = children;
        uint64_t nodeOffset = 0;
        for (; childrenRemaining > 0; --childrenRemaining) {
            const char* ss = symbol;
            bool wrongEdge = false;
            // scan whole edge to get to next edge
            // if edge is longer than target symbol name, don't read past end of symbol name
            char c = *p;
            while ( c != '\0' ) {
                if ( !wrongEdge ) {
                    if ( c != *ss )
                        wrongEdge = true;
                    ++ss;
                }
                ++p;
                c = *p;
            }
            if ( wrongEdge ) {
                // advance to next child
                ++p; // skip over zero terminator
                // skip over uleb128 until last byte is found
                while ( (*p & 0x80) != 0 )
                    ++p;
                ++p; // skip over last byte of uleb128
                if ( p > end ) {
                    diag.error("malformed trie node, child node extends past end of trie\n");
                    return nullptr;
                }
            }
            else {
                 // the symbol so far matches this edge (child)
                // so advance to the child's node
                ++p;
                nodeOffset = read_uleb128(diag, p, end);
                if ( diag.hasError() )
                    return nullptr;
                if ( (nodeOffset == 0) || ( &start[nodeOffset] > end) ) {
                    diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
                    return nullptr;
                }
                symbol = ss;
                break;
            }
        }
        if ( nodeOffset != 0 ) {
            if ( nodeOffset > (uint64_t)(end-start) ) {
                diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
               return nullptr;
            }
            // check for cycles
            for (uint32_t aVisitedNodeOffset : visitedNodeOffsets) {
                if ( aVisitedNodeOffset == nodeOffset ) {
                    diag.error("malformed trie child, cycle to nodeOffset=0x%llX\n", nodeOffset);
                    return nullptr;
                }
            }
            visitedNodeOffsets.push_back((uint32_t)nodeOffset);
            p = &start[nodeOffset];
        }
        else
            p = end;
    }
    return nullptr;
}

bool MachOFile::inCodeSection(uint32_t runtimeOffset) const
{
    // only needed for arm64e code to know to sign pointers
    if ( (this->cputype != CPU_TYPE_ARM64) || (this->maskedCpuSubtype() != CPU_SUBTYPE_ARM64E) )
        return false;

    __block bool result = false;
    uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
    this->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( ((sectInfo.address-baseAddress) <= runtimeOffset) && (runtimeOffset < (sectInfo.address+sectInfo.size-baseAddress)) ) {
            result = ( (sectInfo.flags & S_ATTR_PURE_INSTRUCTIONS) || (sectInfo.flags & S_ATTR_SOME_INSTRUCTIONS) );
            stop = true;
        }
    });
    return result;
}

uint32_t MachOFile::dependentDylibCount(bool* allDepsAreNormalPtr) const
{
    __block uint32_t count = 0;
    __block bool allDepsAreNormal = true;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        ++count;
        if ( isWeak || isReExport || isUpward )
            allDepsAreNormal = false;
    });

    if ( allDepsAreNormalPtr != nullptr )
        *allDepsAreNormalPtr = allDepsAreNormal;
    return count;
}

uint32_t MachOFile::getFixupsLoadCommandFileOffset() const
{
    Diagnostics diag;
    __block uint32_t fileOffset = 0;
    this->forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                fileOffset = (uint32_t)( (uint8_t*)cmd - (uint8_t*)this );
                break;
            case LC_DYLD_CHAINED_FIXUPS:
                fileOffset = (uint32_t)( (uint8_t*)cmd - (uint8_t*)this );
                break;
        }
    });
    if ( diag.hasError() )
        return 0;

    return fileOffset;
}

bool MachOFile::hasInitializer(Diagnostics& diag) const
{
    __block bool result = false;

    // if dylib linked with -init linker option, that initializer is first
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_ROUTINES) || (cmd->cmd == LC_ROUTINES_64) ) {
            result = true;
            stop = true;
        }
    });

    if ( result )
        return true;

    // next any function pointers in mod-init section
    forEachInitializerPointerSection(diag, ^(uint32_t sectionOffset, uint32_t sectionSize, bool& stop) {
        result = true;
        stop = true;
    });

    if ( result )
        return true;

    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS )
            return;
        result = true;
        stop = true;
    });

    return result;
}

void MachOFile::forEachInitializerPointerSection(Diagnostics& diag, void (^callback)(uint32_t sectionOffset, uint32_t sectionSize, bool& stop)) const
{
    const unsigned ptrSize     = pointerSize();
    const uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
    forEachSection(^(const Header::SectionInfo& info, bool& sectStop) {
        if ( (info.flags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS ) {
            if ( (info.size % ptrSize) != 0 ) {
                diag.error("initializer section %.*s/%.*s has bad size",
                           (int)info.segmentName.size(), info.segmentName.data(),
                           (int)info.sectionName.size(), info.sectionName.data());
                sectStop = true;
                return;
            }
            if ( (info.address % ptrSize) != 0 ) {
                diag.error("initializer section %.*s/%.*s is not pointer aligned",
                           (int)info.segmentName.size(), info.segmentName.data(),
                           (int)info.sectionName.size(), info.sectionName.data());
                sectStop = true;
                return;
            }
            callback((uint32_t)(info.address - baseAddress), (uint32_t)info.size, sectStop);
        }
    });
}

uint64_t MachOFile::mappedSize() const
{
    uint64_t vmSpace;
    bool     hasZeroFill;
    analyzeSegmentsLayout(vmSpace, hasZeroFill);
    return vmSpace;
}

void MachOFile::analyzeSegmentsLayout(uint64_t& vmSpace, bool& hasZeroFill) const
{
    __block bool     writeExpansion = false;
    __block uint64_t lowestVmAddr   = 0xFFFFFFFFFFFFFFFFULL;
    __block uint64_t highestVmAddr  = 0;
    __block uint64_t sumVmSizes     = 0;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& segmentInfo, bool& stop) {
        if ( segmentInfo.segmentName == "__PAGEZERO" )
            return;
        if ( segmentInfo.writable() && (segmentInfo.fileSize !=  segmentInfo.vmsize) )
            writeExpansion = true; // zerofill at end of __DATA
        if ( segmentInfo.vmsize == 0 ) {
            // Always zero fill if we have zero-sized segments
            writeExpansion = true;
        }
        if ( segmentInfo.vmaddr < lowestVmAddr )
            lowestVmAddr = segmentInfo.vmaddr;
        if ( segmentInfo.vmaddr+segmentInfo.vmsize > highestVmAddr )
            highestVmAddr = segmentInfo.vmaddr + segmentInfo.vmsize;
        sumVmSizes += segmentInfo.vmsize;
    });
    uint64_t totalVmSpace = (highestVmAddr - lowestVmAddr);
    // LINKEDIT vmSize is not required to be a multiple of page size.  Round up if that is the case
    const uint64_t pageSize = uses16KPages() ? 0x4000 : 0x1000;
    totalVmSpace = (totalVmSpace + (pageSize - 1)) & ~(pageSize - 1);
    bool hasHole = (totalVmSpace != sumVmSizes); // segments not contiguous

    // The aux KC may have __DATA first, in which case we always want to vm_copy to the right place
    bool hasOutOfOrderSegments = false;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
    uint64_t textSegVMAddr = ((const Header*)this)->preferredLoadAddress();
    hasOutOfOrderSegments = textSegVMAddr != lowestVmAddr;
#endif

    vmSpace     = totalVmSpace;
    hasZeroFill = writeExpansion || hasHole || hasOutOfOrderSegments;
}

void MachOFile::forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const
{
    forEachSection(^(const Header::SegmentInfo& segInfo, const Header::SectionInfo& info, bool &stop) {
        if ( (info.flags & SECTION_TYPE) == S_DTRACE_DOF ) {
            callback((uint32_t)(info.address - segInfo.vmaddr));
        }
    });
}

bool MachOFile::hasExportTrie(uint32_t& runtimeOffset, uint32_t& size) const
{
    __block uint64_t textUnslidVMAddr   = 0;
    __block uint64_t linkeditUnslidVMAddr   = 0;
    __block uint64_t linkeditFileOffset     = 0;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( info.segmentName == "__TEXT" ) {
            textUnslidVMAddr = info.vmaddr;
        } else if ( info.segmentName == "__LINKEDIT" ) {
            linkeditUnslidVMAddr = info.vmaddr;
            linkeditFileOffset   = info.fileOffset;
            stop = true;
        }
    });

    Diagnostics diag;
    __block uint32_t fileOffset = ~0U;
    this->forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY: {
                const auto* dyldInfo = (const dyld_info_command*)cmd;
                fileOffset = dyldInfo->export_off;
                size = dyldInfo->export_size;
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                const auto* linkeditCmd = (const linkedit_data_command*)cmd;
                fileOffset = linkeditCmd->dataoff;
                size = linkeditCmd->datasize;
                break;
            }
        }
    });
    if ( diag.hasError() )
        return false;

    if ( fileOffset == ~0U )
        return false;

    runtimeOffset = (uint32_t)((fileOffset - linkeditFileOffset) + (linkeditUnslidVMAddr - textUnslidVMAddr));
    return true;
}

#if !TARGET_OS_EXCLAVEKIT
// Note, this has to match the kernel
static const uint32_t hashPriorities[] = {
    CS_HASHTYPE_SHA1,
    CS_HASHTYPE_SHA256_TRUNCATED,
    CS_HASHTYPE_SHA256,
    CS_HASHTYPE_SHA384,
};

static unsigned int hash_rank(const CS_CodeDirectory *cd)
{
    uint32_t type = cd->hashType;
    for (uint32_t n = 0; n < sizeof(hashPriorities) / sizeof(hashPriorities[0]); ++n) {
        if (hashPriorities[n] == type)
            return n + 1;
    }

    /* not supported */
    return 0;
}

// Note, this does NOT match the kernel.
// On watchOS, in main executables, we will record all cd hashes then make sure
// one of the ones we record matches the kernel.
// This list is only for dylibs where we embed the cd hash in the closure instead of the
// mod time and inode
// This is sorted so that we choose sha1 first when checking dylibs
static const uint32_t hashPriorities_watchOS_dylibs[] = {
    CS_HASHTYPE_SHA256_TRUNCATED,
    CS_HASHTYPE_SHA256,
    CS_HASHTYPE_SHA384,
    CS_HASHTYPE_SHA1
};

static unsigned int hash_rank_watchOS_dylibs(const CS_CodeDirectory *cd)
{
    uint32_t type = cd->hashType;
    for (uint32_t n = 0; n < sizeof(hashPriorities_watchOS_dylibs) / sizeof(hashPriorities_watchOS_dylibs[0]); ++n) {
        if (hashPriorities_watchOS_dylibs[n] == type)
            return n + 1;
    }

    /* not supported */
    return 0;
}

// This calls the callback for all code directories required for a given platform/binary combination.
// On watchOS main executables this is all cd hashes.
// On watchOS dylibs this is only the single cd hash we need (by rank defined by dyld, not the kernel).
// On all other platforms this always returns a single best cd hash (ranked to match the kernel).
// Note the callback parameter is really a CS_CodeDirectory.
void MachOFile::forEachCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen,
                                         void (^callback)(const void* cd)) const
{
    // verify min length of overall code signature
    if ( codeSignLen < sizeof(CS_SuperBlob) )
        return;

    // verify magic at start
    const CS_SuperBlob* codeSuperBlob = (CS_SuperBlob*)codeSigStart;
    if ( codeSuperBlob->magic != htonl(CSMAGIC_EMBEDDED_SIGNATURE) )
        return;

    // verify count of sub-blobs not too large
    uint32_t subBlobCount = htonl(codeSuperBlob->count);
    if ( (codeSignLen-sizeof(CS_SuperBlob))/sizeof(CS_BlobIndex) < subBlobCount )
        return;

    // Note: The kernel sometimes chooses sha1 on watchOS, and sometimes sha256.
    // Embed all of them so that we just need to match any of them
    const bool isWatchOS = ((const Header*)this)->builtForPlatform(Platform::watchOS);
    const bool isMainExecutable = this->isMainExecutable();
    auto hashRankFn = isWatchOS ? &hash_rank_watchOS_dylibs : &hash_rank;

    // walk each sub blob, looking at ones with type CSSLOT_CODEDIRECTORY
    const CS_CodeDirectory* bestCd = nullptr;
    for (uint32_t i=0; i < subBlobCount; ++i) {
        if ( codeSuperBlob->index[i].type == htonl(CSSLOT_CODEDIRECTORY) ) {
            // Ok, this is the regular code directory
        } else if ( codeSuperBlob->index[i].type >= htonl(CSSLOT_ALTERNATE_CODEDIRECTORIES) && codeSuperBlob->index[i].type <= htonl(CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT)) {
            // Ok, this is the alternative code directory
        } else {
            continue;
        }
        uint32_t cdOffset = htonl(codeSuperBlob->index[i].offset);
        // verify offset is not out of range
        if ( cdOffset > (codeSignLen - sizeof(CS_CodeDirectory)) )
            continue;
        const CS_CodeDirectory* cd = (CS_CodeDirectory*)((uint8_t*)codeSuperBlob + cdOffset);
        uint32_t cdLength = htonl(cd->length);
        // verify code directory length not out of range
        if ( cdLength > (codeSignLen - cdOffset) )
            continue;

        // The watch main executable wants to know about all cd hashes
        if ( isWatchOS && isMainExecutable ) {
            callback(cd);
            continue;
        }

        if ( cd->magic == htonl(CSMAGIC_CODEDIRECTORY) ) {
            if ( !bestCd || (hashRankFn(cd) > hashRankFn(bestCd)) )
                bestCd = cd;
        }
    }

    // Note this callback won't happen on watchOS as that one was done in the loop
    if ( bestCd != nullptr )
        callback(bestCd);
}

void MachOFile::forEachCDHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen,
                                             void (^callback)(const uint8_t cdHash[20])) const
{
    forEachCodeDirectoryBlob(codeSigStart, codeSignLen, ^(const void *cdBuffer) {
        const CS_CodeDirectory* cd = (const CS_CodeDirectory*)cdBuffer;
        uint32_t cdLength = htonl(cd->length);
        uint8_t cdHash[20];
//        if ( cd->hashType == CS_HASHTYPE_SHA384 ) {
//            uint8_t digest[CCSHA384_OUTPUT_SIZE];
//            const struct ccdigest_info* di = ccsha384_di();
//            ccdigest_di_decl(di, tempBuf); // declares tempBuf array in stack
//            ccdigest_init(di, tempBuf);
//            ccdigest_update(di, tempBuf, cdLength, cd);
//            ccdigest_final(di, tempBuf, digest);
//            ccdigest_di_clear(di, tempBuf);
//            // cd-hash of sigs that use SHA384 is the first 20 bytes of the SHA384 of the code digest
//            memcpy(cdHash, digest, 20);
//            callback(cdHash);
//            return;
//        }
//        else if ( (cd->hashType == CS_HASHTYPE_SHA256) || (cd->hashType == CS_HASHTYPE_SHA256_TRUNCATED) ) {
//            uint8_t digest[CCSHA256_OUTPUT_SIZE];
//            const struct ccdigest_info* di = ccsha256_di();
//            ccdigest_di_decl(di, tempBuf); // declares tempBuf array in stack
//            ccdigest_init(di, tempBuf);
//            ccdigest_update(di, tempBuf, cdLength, cd);
//            ccdigest_final(di, tempBuf, digest);
//            ccdigest_di_clear(di, tempBuf);
//            // cd-hash of sigs that use SHA256 is the first 20 bytes of the SHA256 of the code digest
//            memcpy(cdHash, digest, 20);
//            callback(cdHash);
//            return;
//        }
//        else if ( cd->hashType == CS_HASHTYPE_SHA1 ) {
//            // compute hash directly into return buffer
//            const struct ccdigest_info* di = ccsha1_di();
//            ccdigest_di_decl(di, tempBuf); // declares tempBuf array in stack
//            ccdigest_init(di, tempBuf);
//            ccdigest_update(di, tempBuf, cdLength, cd);
//            ccdigest_final(di, tempBuf, cdHash);
//            ccdigest_di_clear(di, tempBuf);
//            callback(cdHash);
//            return;
//        }
    });
}
#endif // !TARGET_OS_EXCLAVEKIT

// These are mangled symbols for all the variants of operator new and delete
// which a main executable can define (non-weak) and override the
// weak-def implementation in the OS.
static const char* const sTreatAsWeak[] = {
    "__Znwm", "__ZnwmRKSt9nothrow_t",
    "__Znam", "__ZnamRKSt9nothrow_t",
    "__ZdlPv", "__ZdlPvRKSt9nothrow_t", "__ZdlPvm",
    "__ZdaPv", "__ZdaPvRKSt9nothrow_t", "__ZdaPvm",
    "__ZnwmSt11align_val_t", "__ZnwmSt11align_val_tRKSt9nothrow_t",
    "__ZnamSt11align_val_t", "__ZnamSt11align_val_tRKSt9nothrow_t",
    "__ZdlPvSt11align_val_t", "__ZdlPvSt11align_val_tRKSt9nothrow_t", "__ZdlPvmSt11align_val_t",
    "__ZdaPvSt11align_val_t", "__ZdaPvSt11align_val_tRKSt9nothrow_t", "__ZdaPvmSt11align_val_t",
    "__ZnwmSt19__type_descriptor_t", "__ZnamSt19__type_descriptor_t"
};

void MachOFile::forEachTreatAsWeakDef(void (^handler)(const char* symbolName))
{
    for (const char*  sym : sTreatAsWeak)
        handler(sym);
}

MachOFile::PointerMetaData::PointerMetaData()
{
    this->diversity           = 0;
    this->high8               = 0;
    this->authenticated       = 0;
    this->key                 = 0;
    this->usesAddrDiversity   = 0;
}

MachOFile::PointerMetaData::PointerMetaData(const ChainedFixupPointerOnDisk* fixupLoc, uint16_t pointer_format)
{
    this->diversity           = 0;
    this->high8               = 0;
    this->authenticated       = 0;
    this->key                 = 0;
    this->usesAddrDiversity   = 0;
    switch ( pointer_format ) {
        case DYLD_CHAINED_PTR_ARM64E:
        case DYLD_CHAINED_PTR_ARM64E_KERNEL:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
        case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            this->authenticated = fixupLoc->arm64e.authRebase.auth;
            if ( this->authenticated ) {
                this->key               = fixupLoc->arm64e.authRebase.key;
                this->usesAddrDiversity = fixupLoc->arm64e.authRebase.addrDiv;
                this->diversity         = fixupLoc->arm64e.authRebase.diversity;
            }
            else if ( fixupLoc->arm64e.bind.bind == 0 ) {
                this->high8             = fixupLoc->arm64e.rebase.high8;
            }
            break;
        case DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE:
            this->authenticated = fixupLoc->cache64e.auth.auth;
            if ( this->authenticated ) {
                this->key               = fixupLoc->cache64e.auth.keyIsData ? 2 : 0; // true -> DA (2), false -> IA (0)
                this->usesAddrDiversity = fixupLoc->cache64e.auth.addrDiv;
                this->diversity         = fixupLoc->cache64e.auth.diversity;
            }
            else {
                this->high8 = fixupLoc->cache64e.regular.high8;
            }
            break;
        case DYLD_CHAINED_PTR_64:
        case DYLD_CHAINED_PTR_64_OFFSET:
            if ( fixupLoc->generic64.bind.bind == 0 )
                this->high8             = fixupLoc->generic64.rebase.high8;
            break;
    }
}

bool MachOFile::PointerMetaData::operator==(const PointerMetaData& other) const
{
    return (this->diversity == other.diversity)
        && (this->high8 == other.high8)
        && (this->authenticated == other.authenticated)
        && (this->key == other.key)
        && (this->usesAddrDiversity == other.usesAddrDiversity);
}

#if !SUPPORT_VM_LAYOUT || BUILDING_UNIT_TESTS || BUILDING_DYLD_SYMBOLS_CACHE
bool MachOFile::getLinkeditLayout(Diagnostics& diag, mach_o::LinkeditLayout& layout) const
{
    // Note, in file layout all linkedit offsets are just file offsets.
    // It is essential no-one calls this on a MachOLoaded or MachOAnalyzer

    // FIXME: Other load commands
    this->forEachLoadCommand(diag, ^(const load_command *cmd, bool &stop) {
        switch ( cmd->cmd ) {
            case LC_SYMTAB: {
                const symtab_command* symTabCmd = (const symtab_command*)cmd;

                // Record that we found a LC_SYMTAB
                layout.hasSymTab = true;

                // NList
                uint64_t nlistEntrySize  = this->is64() ? sizeof(struct nlist_64) : sizeof(struct nlist);
                layout.symbolTable.fileOffset       = symTabCmd->symoff;
                layout.symbolTable.buffer           = (uint8_t*)this + symTabCmd->symoff;
                layout.symbolTable.bufferSize       = (uint32_t)(symTabCmd->nsyms * nlistEntrySize);
                layout.symbolTable.entryCount       = symTabCmd->nsyms;
                layout.symbolTable.hasLinkedit      = true;

                // Symbol strings
                layout.symbolStrings.fileOffset     = symTabCmd->stroff;
                layout.symbolStrings.buffer         = (uint8_t*)this + symTabCmd->stroff;
                layout.symbolStrings.bufferSize     = symTabCmd->strsize;
                layout.symbolStrings.hasLinkedit    = true;
                break;
            }
            case LC_DYSYMTAB: {
                const dysymtab_command* dynSymTabCmd = (const dysymtab_command*)cmd;

                // Record that we found a LC_DYSYMTAB
                layout.hasDynSymTab = true;

                // Local relocs
                layout.localRelocs.fileOffset          = dynSymTabCmd->locreloff;
                layout.localRelocs.buffer              = (uint8_t*)this + dynSymTabCmd->locreloff;
                layout.localRelocs.bufferSize          = 0;         // Use entryCount instead
                layout.localRelocs.entryIndex          = 0;         // Use buffer instead
                layout.localRelocs.entryCount          = dynSymTabCmd->nlocrel;
                layout.localRelocs.hasLinkedit         = true;

                // Extern relocs
                layout.externRelocs.fileOffset          = dynSymTabCmd->extreloff;
                layout.externRelocs.buffer              = (uint8_t*)this + dynSymTabCmd->extreloff;
                layout.externRelocs.bufferSize          = 0;         // Use entryCount instead
                layout.externRelocs.entryIndex          = 0;         // Use buffer instead
                layout.externRelocs.entryCount          = dynSymTabCmd->nextrel;
                layout.externRelocs.hasLinkedit         = true;

                // Indirect symbol table
                layout.indirectSymbolTable.fileOffset   = dynSymTabCmd->indirectsymoff;
                layout.indirectSymbolTable.buffer       = (uint8_t*)this + dynSymTabCmd->indirectsymoff;
                layout.indirectSymbolTable.bufferSize   = 0;         // Use entryCount instead
                layout.indirectSymbolTable.entryIndex   = 0;         // Use buffer instead
                layout.indirectSymbolTable.entryCount   = dynSymTabCmd->nindirectsyms;
                layout.indirectSymbolTable.hasLinkedit  = true;

                // Locals
                layout.localSymbolTable.fileOffset     = 0;         // unused
                layout.localSymbolTable.buffer         = nullptr;   // Use entryIndex instead
                layout.localSymbolTable.bufferSize     = 0;         // Use entryCount instead
                layout.localSymbolTable.entryIndex     = dynSymTabCmd->ilocalsym;
                layout.localSymbolTable.entryCount     = dynSymTabCmd->nlocalsym;
                layout.localSymbolTable.hasLinkedit    = true;

                // Globals
                layout.globalSymbolTable.fileOffset     = 0;         // unused
                layout.globalSymbolTable.buffer         = nullptr;   // Use entryIndex instead
                layout.globalSymbolTable.bufferSize     = 0;         // Use entryCount instead
                layout.globalSymbolTable.entryIndex     = dynSymTabCmd->iextdefsym;
                layout.globalSymbolTable.entryCount     = dynSymTabCmd->nextdefsym;
                layout.globalSymbolTable.hasLinkedit    = true;

                // Imports
                layout.undefSymbolTable.fileOffset     = 0;         // unused
                layout.undefSymbolTable.buffer         = nullptr;   // Use entryIndex instead
                layout.undefSymbolTable.bufferSize     = 0;         // Use entryCount instead
                layout.undefSymbolTable.entryIndex     = dynSymTabCmd->iundefsym;
                layout.undefSymbolTable.entryCount     = dynSymTabCmd->nundefsym;
                layout.undefSymbolTable.hasLinkedit    = true;
                break;
            }
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY: {
                const dyld_info_command* linkeditCmd = (const dyld_info_command*)cmd;

                // Record what kind of DYLD_INFO we found
                layout.dyldInfoCmd = cmd->cmd;

                // Rebase
                layout.rebaseOpcodes.fileOffset         = linkeditCmd->rebase_off;
                layout.rebaseOpcodes.buffer             = (uint8_t*)this + linkeditCmd->rebase_off;
                layout.rebaseOpcodes.bufferSize         = linkeditCmd->rebase_size;
                layout.rebaseOpcodes.hasLinkedit        = true;

                // Bind
                layout.regularBindOpcodes.fileOffset    = linkeditCmd->bind_off;
                layout.regularBindOpcodes.buffer        = (uint8_t*)this + linkeditCmd->bind_off;
                layout.regularBindOpcodes.bufferSize    = linkeditCmd->bind_size;
                layout.regularBindOpcodes.hasLinkedit   = true;

                // Lazy bind
                layout.lazyBindOpcodes.fileOffset       = linkeditCmd->lazy_bind_off;
                layout.lazyBindOpcodes.buffer           = (uint8_t*)this + linkeditCmd->lazy_bind_off;
                layout.lazyBindOpcodes.bufferSize       = linkeditCmd->lazy_bind_size;
                layout.lazyBindOpcodes.hasLinkedit      = true;

                // Weak bind
                layout.weakBindOpcodes.fileOffset       = linkeditCmd->weak_bind_off;
                layout.weakBindOpcodes.buffer           = (uint8_t*)this + linkeditCmd->weak_bind_off;
                layout.weakBindOpcodes.bufferSize       = linkeditCmd->weak_bind_size;
                layout.weakBindOpcodes.hasLinkedit      = true;

                // Export trie
                layout.exportsTrie.fileOffset           = linkeditCmd->export_off;
                layout.exportsTrie.buffer               = (uint8_t*)this + linkeditCmd->export_off;
                layout.exportsTrie.bufferSize           = linkeditCmd->export_size;
                layout.exportsTrie.hasLinkedit          = true;
                break;
            }
            case LC_DYLD_CHAINED_FIXUPS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.chainedFixups.fileOffset         = linkeditCmd->dataoff;
                layout.chainedFixups.buffer             = (uint8_t*)this + linkeditCmd->dataoff;
                layout.chainedFixups.bufferSize         = linkeditCmd->datasize;
                layout.chainedFixups.entryCount         = 0; // Not needed here
                layout.chainedFixups.hasLinkedit        = true;
                layout.chainedFixups.cmd                = linkeditCmd;
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.exportsTrie.fileOffset           = linkeditCmd->dataoff;
                layout.exportsTrie.buffer               = (uint8_t*)this + linkeditCmd->dataoff;
                layout.exportsTrie.bufferSize           = linkeditCmd->datasize;
                layout.exportsTrie.entryCount           = 0; // Not needed here
                layout.exportsTrie.hasLinkedit          = true;
                break;
            }
            case LC_SEGMENT_SPLIT_INFO: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.splitSegInfo.fileOffset           = linkeditCmd->dataoff;
                layout.splitSegInfo.buffer               = (uint8_t*)this + linkeditCmd->dataoff;
                layout.splitSegInfo.bufferSize           = linkeditCmd->datasize;
                layout.splitSegInfo.entryCount           = 0; // Not needed here
                layout.splitSegInfo.hasLinkedit          = true;
                break;
            }
            case LC_FUNCTION_STARTS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.functionStarts.fileOffset           = linkeditCmd->dataoff;
                layout.functionStarts.buffer               = (uint8_t*)this + linkeditCmd->dataoff;
                layout.functionStarts.bufferSize           = linkeditCmd->datasize;
                layout.functionStarts.entryCount           = 0; // Not needed here
                layout.functionStarts.hasLinkedit          = true;
                break;
            }
            case LC_DATA_IN_CODE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.dataInCode.fileOffset    = linkeditCmd->dataoff;
                layout.dataInCode.buffer        = (uint8_t*)this + linkeditCmd->dataoff;
                layout.dataInCode.bufferSize    = linkeditCmd->datasize;
                layout.dataInCode.entryCount    = 0; // Not needed here
                layout.dataInCode.hasLinkedit   = true;
                break;
            }
            case LC_CODE_SIGNATURE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.codeSignature.fileOffset    = linkeditCmd->dataoff;
                layout.codeSignature.buffer        = (uint8_t*)this + linkeditCmd->dataoff;
                layout.codeSignature.bufferSize    = linkeditCmd->datasize;
                layout.codeSignature.entryCount    = 0; // Not needed here
                layout.codeSignature.hasLinkedit   = true;
                break;
            }
        }
    });

    return true;
}

void MachOFile::withFileLayout(Diagnostics &diag, void (^callback)(const mach_o::Layout &layout)) const
{
    // Use the fixups from the source dylib
    mach_o::LinkeditLayout linkedit;
    if ( !this->getLinkeditLayout(diag, linkedit) ) {
        diag.error("Couldn't get dylib layout");
        return;
    }

    uint32_t numSegments = ((const Header*)this)->segmentCount();
    BLOCK_ACCCESSIBLE_ARRAY(mach_o::SegmentLayout, segmentLayout, numSegments);
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
        mach_o::SegmentLayout segment;
        segment.vmAddr      = info.vmaddr;
        segment.vmSize      = info.vmsize;
        segment.fileOffset  = info.fileOffset;
        segment.fileSize    = info.fileSize;
        segment.buffer      = (uint8_t*)this + info.fileOffset;
        segment.protections = info.initProt;

        segment.kind        = mach_o::SegmentLayout::Kind::unknown;
        if ( info.segmentName == "__TEXT" ) {
            segment.kind    = mach_o::SegmentLayout::Kind::text;
        } else if ( info.segmentName == "__LINKEDIT" ) {
            segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
        }

        segmentLayout[info.segmentIndex] = segment;
    });

    mach_o::Layout layout(this, { &segmentLayout[0], &segmentLayout[numSegments] }, linkedit);
    callback(layout);
}
#endif // !SUPPORT_VM_LAYOUT

bool MachOFile::enforceFormat(Malformed kind) const
{
    // TODO: Add a mapping from generic releases to platform versions
#if BUILDING_DYLDINFO || BUILDING_APP_CACHE_UTIL
    // HACK: If we are the kernel, we have a different format to enforce
    if ( isFileSet() ) {
        bool result = false;
        switch (kind) {
        case Malformed::linkeditOrder:
        case Malformed::linkeditAlignment:
        case Malformed::dyldInfoAndlocalRelocs:
            result = true;
            break;
        case Malformed::segmentOrder:
        // The aux KC has __DATA first
            result = false;
            break;
        case Malformed::linkeditPermissions:
        case Malformed::executableData:
        case Malformed::writableData:
        case Malformed::codeSigAlignment:
        case Malformed::sectionsAddrRangeWithinSegment:
        case Malformed::loaderPathsAreReal:
        case Malformed::mainExecInDyldCache:
            result = true;
            break;
        case Malformed::noLinkedDylibs:
        case Malformed::textPermissions:
            // The kernel has its own __TEXT_EXEC for executable memory
            result = false;
            break;
        case Malformed::noUUID:
        case Malformed::zerofillSwiftMetadata:
        case Malformed::sdkOnOrAfter2021:
        case Malformed::sdkOnOrAfter2022:
            result = true;
            break;
        }
        return result;
    }

    if ( isStaticExecutable() ) {
        bool result = false;
        switch (kind) {
        case Malformed::linkeditOrder:
        case Malformed::linkeditAlignment:
        case Malformed::dyldInfoAndlocalRelocs:
            result = true;
            break;
        case Malformed::segmentOrder:
        case Malformed::textPermissions:
            result = false;
            break;
        case Malformed::linkeditPermissions:
        case Malformed::executableData:
        case Malformed::codeSigAlignment:
        case Malformed::sectionsAddrRangeWithinSegment:
        case Malformed::loaderPathsAreReal:
        case Malformed::mainExecInDyldCache:
            result = true;
            break;
        case Malformed::noLinkedDylibs:
        case Malformed::writableData:
        case Malformed::noUUID:
        case Malformed::zerofillSwiftMetadata:
        case Malformed::sdkOnOrAfter2021:
        case Malformed::sdkOnOrAfter2022:
            // The kernel has __DATA_CONST marked as r/o
            result = false;
            break;
        }
        return result;
    }

#endif

    __block bool result = false;
    mach_o::PlatformAndVersions pvs = ((const Header*)this)->platformAndVersions();
    pvs.unzip(^(mach_o::PlatformAndVersions p) {
        if ( p.platform == Platform::macOS ) {
            switch (kind) {
                case Malformed::linkeditOrder:
                case Malformed::linkeditAlignment:
                case Malformed::dyldInfoAndlocalRelocs:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x000A0E00) // macOS 10.14
                        result = true;
                    break;
                case Malformed::segmentOrder:
                case Malformed::linkeditPermissions:
                case Malformed::textPermissions:
                case Malformed::executableData:
                case Malformed::writableData:
                case Malformed::codeSigAlignment:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x000A0F00) // macOS 10.15
                        result = true;
                    break;
                case Malformed::sectionsAddrRangeWithinSegment:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x000A1000) // macOS 10.16
                        result = true;
                    break;
                case Malformed::noLinkedDylibs:
                case Malformed::loaderPathsAreReal:
                case Malformed::mainExecInDyldCache:
                case Malformed::zerofillSwiftMetadata:
                case Malformed::sdkOnOrAfter2021:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x000D0000) // macOS 13.0
                        result = true;
                    break;
                case Malformed::noUUID:
                case Malformed::sdkOnOrAfter2022:
                    if (p.sdk.value() >= 0x000E0000) // macOS 14.0  FIXME
                        result = true;
                    break;
            }
        } else if ( p.platform == Platform::iOS || p.platform == Platform::tvOS || p.platform == Platform::macCatalyst ) {
            switch (kind) {
                case Malformed::linkeditOrder:
                case Malformed::dyldInfoAndlocalRelocs:
                case Malformed::textPermissions:
                case Malformed::executableData:
                case Malformed::writableData:
                    result = true;
                    break;
                case Malformed::linkeditAlignment:
                case Malformed::segmentOrder:
                case Malformed::linkeditPermissions:
                case Malformed::codeSigAlignment:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x000D0000) // iOS 13
                        result = true;
                    break;
                case Malformed::sectionsAddrRangeWithinSegment:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x000E0000) // iOS 14
                        result = true;
                    break;
                case Malformed::noLinkedDylibs:
                case Malformed::loaderPathsAreReal:
                case Malformed::mainExecInDyldCache:
                case Malformed::zerofillSwiftMetadata:
                case Malformed::sdkOnOrAfter2021:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x00100000) // iOS 16
                        result = true;
                    break;
                case Malformed::noUUID:
                case Malformed::sdkOnOrAfter2022:
                    if (p.sdk.value() >= 0x00110000) // iOS 17.0 FIXME
                        result = true;
                    break;
            }
        } else if ( p.platform == Platform::watchOS ) {
            switch (kind) {
                case Malformed::linkeditOrder:
                case Malformed::dyldInfoAndlocalRelocs:
                case Malformed::textPermissions:
                case Malformed::executableData:
                case Malformed::writableData:
                    result = true;
                    break;
                case Malformed::linkeditAlignment:
                case Malformed::segmentOrder:
                case Malformed::linkeditPermissions:
                case Malformed::codeSigAlignment:
                case Malformed::sectionsAddrRangeWithinSegment:
                case Malformed::noLinkedDylibs:
                case Malformed::loaderPathsAreReal:
                case Malformed::mainExecInDyldCache:
                case Malformed::zerofillSwiftMetadata:
                case Malformed::sdkOnOrAfter2021:
                    // enforce these checks on new binaries only
                    if (p.sdk.value() >= 0x00090000) // watchOS 9
                        result = true;
                    break;
                case Malformed::noUUID:
                case Malformed::sdkOnOrAfter2022:
                    if (p.sdk.value() >= 0x000A0000) // watchOS 10 FIXME
                        result = true;
                    break;
            }
        } else if ( p.platform == Platform::driverKit ) {
            result = true;
        } else if ( p.platform == Platform::visionOS || p.platform == Platform::visionOS_simulator ) {
            result = true; // do all checks by default
            if ( kind == Malformed::sdkOnOrAfter2022 ) {
                if (p.sdk.value() < 0x00020000) // visionOS 2.0 FIXME
                    result = false;
            }
        }
        
        // if binary is so old, there is no platform info, don't enforce malformed errors
        else if ( p.platform.empty() ) {
            result = false;
        } else {
            result = true;
        }
    });
    
    return result;
}

bool MachOFile::validSegments(Diagnostics& diag, const char* path, size_t fileLen) const
{
    // check segment load command size
    __block bool badSegmentLoadCommand = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command_64);
            if ( sectionsSpace < 0 ) {
               diag.error("in '%s' load command size too small for LC_SEGMENT_64", path);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section_64)) != 0 ) {
               diag.error("in '%s' segment load command size 0x%X will not fit whole number of sections", path, cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (int32_t)(seg->nsects * sizeof(section_64)) ) {
               diag.error("in '%s' load command size 0x%X does not match nsects %d", path, cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( greaterThanAddOrOverflow(seg->fileoff, seg->filesize, fileLen) ) {
                diag.error("in '%s' segment load command content extends beyond end of file", path);
                badSegmentLoadCommand = true;
                stop = true;
            }
            else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.error("in '%s' segment '%s' filesize exceeds vmsize", path, seg->segname);
                badSegmentLoadCommand = true;
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command);
            if ( sectionsSpace < 0 ) {
               diag.error("in '%s' load command size too small for LC_SEGMENT", path);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section)) != 0 ) {
               diag.error("in '%s' segment load command size 0x%X will not fit whole number of sections", path, cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (int32_t)(seg->nsects * sizeof(section)) ) {
               diag.error("in '%s' load command size 0x%X does not match nsects %d", path, cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.error("in '%s' segment  '%s' filesize exceeds vmsize", path, seg->segname);
                badSegmentLoadCommand = true;
                stop = true;
            }
        }
    });
     if ( badSegmentLoadCommand )
         return false;

    // check mapping permissions of segments
    __block bool badPermissions = false;
    __block bool badSize        = false;
    __block bool hasTEXT        = false;
    __block bool hasLINKEDIT    = false;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( info.segmentName == "__TEXT" ) {
            if ( (info.initProt != (VM_PROT_READ|VM_PROT_EXECUTE)) && enforceFormat(Malformed::textPermissions) ) {
                diag.error("in '%s' __TEXT segment permissions is not 'r-x'", path);
                badPermissions = true;
                stop = true;
            }
            hasTEXT = true;
        }
        else if ( info.segmentName == "__LINKEDIT" ) {
            if ( (info.initProt != VM_PROT_READ) && enforceFormat(Malformed::linkeditPermissions) ) {
                diag.error("in '%s' __LINKEDIT segment permissions is not 'r--'", path);
                badPermissions = true;
                stop = true;
            }
            hasLINKEDIT = true;
        }
        else if ( (info.initProt & 0xFFFFFFF8) != 0 ) {
            diag.error("in '%s' %.*s segment permissions has invalid bits set", path,
                       (int)info.segmentName.size(), info.segmentName.data());
            badPermissions = true;
            stop = true;
        }
        if ( greaterThanAddOrOverflow(info.fileOffset, info.fileSize, fileLen) ) {
            diag.error("in '%s' %.*s segment content extends beyond end of file", path,
                       (int)info.segmentName.size(), info.segmentName.data());
            badSize = true;
            stop = true;
        }
        if ( is64() ) {
            if ( info.vmaddr + info.vmsize < info.vmaddr ) {
                diag.error("in '%s' %.*s segment vm range wraps", path,
                           (int)info.segmentName.size(), info.segmentName.data());
                badSize = true;
                stop = true;
            }
       }
       else {
            if ( (uint32_t)(info.vmaddr + info.vmsize) < (uint32_t)(info.vmaddr) ) {
                diag.error("in '%s' %.*s segment vm range wraps", path,
                           (int)info.segmentName.size(), info.segmentName.data());
                badSize = true;
                stop = true;
            }
       }
    });
    if ( badPermissions || badSize )
        return false;
    if ( !hasTEXT ) {
        diag.error("in '%s' missing __TEXT segment", path);
        return false;
    }
    if ( !hasLINKEDIT && !this->isPreload() ) {
       diag.error("in '%s' missing __LINKEDIT segment", path);
       return false;
    }

    // check for overlapping segments
    __block bool badSegments = false;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info1, bool& stop1) {
        uint64_t seg1vmEnd   = info1.vmaddr + info1.vmsize;
        uint64_t seg1FileEnd = info1.fileOffset + info1.fileSize;
        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info2, bool& stop2) {
            if ( info1.segmentIndex == info2.segmentIndex )
                return;
            uint64_t seg2vmEnd   = info2.vmaddr + info2.vmsize;
            uint64_t seg2FileEnd = info2.fileOffset + info2.fileSize;
            if ( ((info2.vmaddr <= info1.vmaddr) && (seg2vmEnd > info1.vmaddr) && (seg1vmEnd > info1.vmaddr )) || ((info2.vmaddr >= info1.vmaddr ) && (info2.vmaddr < seg1vmEnd) && (seg2vmEnd > info2.vmaddr)) ) {
                diag.error("in '%s' segment %.*s vm range overlaps segment %.*s", path,
                           (int)info1.segmentName.size(), info1.segmentName.data(),
                           (int)info2.segmentName.size(), info2.segmentName.data());
                badSegments = true;
                stop1 = true;
                stop2 = true;
            }
             if ( ((info2.fileOffset  <= info1.fileOffset) && (seg2FileEnd > info1.fileOffset) && (seg1FileEnd > info1.fileOffset)) || ((info2.fileOffset  >= info1.fileOffset) && (info2.fileOffset  < seg1FileEnd) && (seg2FileEnd > info2.fileOffset )) ) {
                 if ( !inDyldCache() ) {
                     // HACK: Split shared caches might put the __TEXT in a SubCache, then the __DATA in a later SubCache.
                     // The file offsets are in to each SubCache file, which means that they might overlap
                     // For now we have no choice but to disable this error
                     diag.error("in '%s' segment %.*s file content overlaps segment %.*s", path,
                                (int)info1.segmentName.size(), info1.segmentName.data(),
                                (int)info2.segmentName.size(), info2.segmentName.data());
                     badSegments = true;
                     stop1 = true;
                     stop2 = true;
                 }
            }
            if ( (info1.segmentIndex < info2.segmentIndex) && !stop1 ) {
                if ( (info1.vmaddr > info2.vmaddr) || ((info1.fileOffset > info2.fileOffset ) && (info1.fileOffset != 0) && (info2.fileOffset  != 0)) ){
                    if ( !inDyldCache() && enforceFormat(Malformed::segmentOrder) && !isStaticExecutable() ) {
                        // <rdar://80084852> whitelist go libraries __DWARF segments
                        if ( info1.segmentName != "__DWARF" && info2.segmentName != "__DWARF" ) {
                            // dyld cache __DATA_* segments are moved around
                            // The static kernel also has segments with vmAddr's before __TEXT
                            diag.error("in '%s' segment load commands out of order with respect to layout for %.*s and %.*s", path,
                                       (int)info1.segmentName.size(), info1.segmentName.data(),
                                       (int)info2.segmentName.size(), info2.segmentName.data());
                            badSegments = true;
                            stop1 = true;
                            stop2 = true;
                        }
                    }
                }
            }
        });
    });
    if ( badSegments )
        return false;

    // check sections are within segment
    __block bool badSections = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            const section_64* const sectionsStart = (section_64*)((char*)seg + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section_64* sect=sectionsStart; (sect < sectionsEnd); ++sect) {
                if ( (int64_t)(sect->size) < 0 ) {
                    diag.error("in '%s' section '%s' size too large 0x%llX", path, sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.error("in '%s' section '%s' start address 0x%llX is before containing segment's address 0x%0llX", path, sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    bool ignoreError = !enforceFormat(Malformed::sectionsAddrRangeWithinSegment);
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
                    if ( (seg->vmsize == 0) && !strcmp(seg->segname, "__CTF") )
                        ignoreError = true;
#endif
                    if ( !ignoreError ) {
                        diag.error("in '%s' section '%s' end address 0x%llX is beyond containing segment's end address 0x%0llX", path, sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                        badSections = true;
                    }
                }
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            const section* const sectionsStart = (section*)((char*)seg + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
               if ( (int64_t)(sect->size) < 0 ) {
                    diag.error("in '%s' section %s size too large 0x%X", path, sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.error("in '%s' section %s start address 0x%X is before containing segment's address 0x%0X", path,  sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    diag.error("in '%s' section %s end address 0x%X is beyond containing segment's end address 0x%0X", path, sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                    badSections = true;
                }
            }
        }
    });

    return !badSections;
}

} // namespace dyld3

