# Cross-compile for Android arm64 (Meta Quest 3) using the Android NDK.
# Requires ANDROID_NDK env var or -DANDROID_NDK=/path (e.g. ~/Library/Android/sdk/ndk/<version>).
if(NOT DEFINED ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK})
        set(ANDROID_NDK "$ENV{ANDROID_NDK}")
    elseif(DEFINED ENV{ANDROID_HOME})
        file(GLOB _NDK_CANDIDATES "$ENV{ANDROID_HOME}/ndk/*")
        list(SORT _NDK_CANDIDATES)
        list(REVERSE _NDK_CANDIDATES)
        if(_NDK_CANDIDATES)
            list(GET _NDK_CANDIDATES 0 ANDROID_NDK)
        endif()
    endif()
endif()

if(NOT ANDROID_NDK OR NOT EXISTS "${ANDROID_NDK}")
    message(FATAL_ERROR
        "Android NDK not found.\n"
        "Install it via Android Studio (SDK Manager → SDK Tools → NDK) then set:\n"
        "  export ANDROID_NDK=~/Library/Android/sdk/ndk/<version>")
endif()

set(ANDROID_ABI arm64-v8a)
set(ANDROID_PLATFORM android-29)   # API 29 = Android 10; covers all Meta Quest devices
set(ANDROID_STL c++_static)

include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")

# Override output platform name so the library is named libhop-physics.android.*.so
set(GOLDNET_PLATFORM_NAME "android" CACHE STRING "" FORCE)
