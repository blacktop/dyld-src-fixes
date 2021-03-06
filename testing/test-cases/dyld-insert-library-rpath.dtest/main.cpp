
// BOOT_ARGS: dyld_flags=2

// BUILD:  $CXX main.cpp -std=c++11 -o $BUILD_DIR/rpath_insert_main.exe  -Wl,-rpath,$RUN_DIR/lib
// BUILD:  $CC foo.c -dynamiclib -install_name $RUN_DIR/libfoo.dylib -o $BUILD_DIR/lib/libfoo.dylib
// BUILD:  $CC bar.c -dynamiclib -install_name $RUN_DIR/libbar.dylib -o $BUILD_DIR/libbar.dylib
// BUILD:  $CC baz.c -dynamiclib -install_name $RUN_DIR/libbaz.dylib -o $BUILD_DIR/libbaz.dylib
// BUILD:  $DYLD_ENV_VARS_ENABLE $BUILD_DIR/rpath_insert_main.exe

// Load foo with @rpath, bar with @executable_path, and baz with @loader_path

// Note, dyld2 only supports DYLD_INSERT_LIBRARIES with @executable path so we expect failures on @rpath and @loader_path

// RUN:  DYLD_INSERT_LIBRARIES="@rpath/libfoo.dylib"  			DYLD_AMFI_FAKE=0xFF ./rpath_insert_main.exe libfoo.dylib
// RUN:  DYLD_INSERT_LIBRARIES="@executable_path/libbar.dylib"                      ./rpath_insert_main.exe libbar.dylib
// RUN:  DYLD_INSERT_LIBRARIES="@loader_path/libbaz.dylib"  	DYLD_AMFI_FAKE=0xFF ./rpath_insert_main.exe libbaz.dylib

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <mach-o/dyld_priv.h>

#include "test_support.h"

bool gFoundLibrary = false;
const char* gLibraryName = NULL;

bool wasImageLoaded(const char* libraryName) {
	gFoundLibrary = false;
	gLibraryName = libraryName;
	_dyld_register_for_image_loads([](const mach_header* mh, const char* path, bool unloadable) {
		if ( strstr(path, gLibraryName) != NULL ) {
			gFoundLibrary = true;
		}
	});
	return gFoundLibrary;
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    if (argc != 2) {
        FAIL("Expected library name");
    }

    bool expectInsertFailure = getenv("DYLD_AMFI_FAKE") != NULL;

    if (wasImageLoaded(argv[1])) {   
        // Image was loaded, but make sure that is what we wanted to happen
        if ( expectInsertFailure ) {
            FAIL("Expected insert to fail for '%s'", argv[1]);
        }
    } else {
        // Image was not loaded, so make sure we are ok with that
        if ( !expectInsertFailure ) {
            FAIL("Expected insert to pass for '%s'", argv[1]);
        }
    }

    PASS("Success");
}
