cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(PixCarsVita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "PixCars Vita")
set(VITA_TITLEID  "PXCV00001")
set(VITA_VERSION  "00.01")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu99 -Wall -ffast-math -mfpu=neon -mcpu=cortex-a9")

add_executable(${PROJECT_NAME}
  src/main.c
  src/vcm.c
  src/scene.c
  src/car.c
  src/light.c
  src/camera.c
  src/audio.c
  src/movie.c
  src/ui.c
)

target_link_libraries(${PROJECT_NAME}
  vitaGL
  SceAudio_stub
  SceAvPlayer_stub
  vitashark
  SceShaccCgExt
  mathneon
  taihen_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceSysmodule_stub
  SceIme_stub
  SceAppUtil_stub
  SceTouch_stub
  SceLibKernel_stub
  stdc++
  m
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
