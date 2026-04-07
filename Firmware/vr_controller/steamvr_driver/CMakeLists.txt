cmake_minimum_required(VERSION 3.16)
project(driver_diyvr)

# OpenVR SDK yolunu ayarla
# İndirdikten sonra buraya yaz:
set(OPENVR_SDK_DIR "C:/openvr" CACHE PATH "OpenVR SDK dizini")

add_library(driver_diyvr SHARED
    src/driver_diyvr.cpp
)

target_include_directories(driver_diyvr PRIVATE
    ${OPENVR_SDK_DIR}/headers
)

# OpenVR lib
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_libraries(driver_diyvr
        ${OPENVR_SDK_DIR}/lib/win64/openvr_api.lib
    )
    set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/diyvr/bin/win64)
else()
    target_link_libraries(driver_diyvr
        ${OPENVR_SDK_DIR}/lib/win32/openvr_api.lib
    )
    set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/diyvr/bin/win32)
endif()

# DLL çıktı yeri
set_target_properties(driver_diyvr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIR}
)

# Windows-specific
target_compile_definitions(driver_diyvr PRIVATE
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
