set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(RV1106_CROSS arm-rockchip830-linux-uclibcgnueabihf)
set(CMAKE_C_COMPILER   ${RV1106_CROSS}-gcc)
set(CMAKE_CXX_COMPILER ${RV1106_CROSS}-g++)
set(CMAKE_AR           ${RV1106_CROSS}-ar CACHE FILEPATH "")
set(CMAKE_RANLIB       ${RV1106_CROSS}-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP        ${RV1106_CROSS}-strip CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# uClibc <assert.h> lacks the C11 static_assert macro; picotls trips -Werror
# on 32-bit pointer<->uint64 casts in its logging macros.
# Per-function/data sections let the application link drop unused code (--gc-sections)
set(CMAKE_C_FLAGS_INIT "-Dstatic_assert=_Static_assert -Wno-error=pointer-to-int-cast -Wno-error=int-to-pointer-cast -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections")
