add_library(rpmsg_netif
    freertos/rpmsg_eth.c
    freertos/rpmsg_eth.h
    freertos/network.c
)

target_include_directories(rpmsg_netif
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos
)

target_link_libraries(rpmsg_netif
    PRIVATE
    lwipcore
    lwipallapps
)

set (LWIP_DEFINITIONS LWIP_DEBUG=1)

add_compile_definitions(
    IN_ADDR_T_DEFINED=1
    LWIP_PROVIDE_ERRNO=1
)

set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lwip211_v1_8/src/lwip-2.1.1)
set (LWIP_INCLUDE_DIRS
    ${LWIP_DIR}/src/include
    ${LWIP_DIR}/contrib
    lwip_arch_xilinx/include
)

include(${LWIP_DIR}/src/Filelists.cmake)



add_library(lwip_arch
    lwip_arch_xilinx/sys_arch.c
    lwip_arch_xilinx/sys_arch_raw.c
)

target_include_directories(lwip_arch PUBLIC
    ${LWIP_INCLUDE_DIRS}
)

target_link_libraries(lwip_arch PUBLIC
    xilinx_platform
)

target_link_libraries(lwipcore PUBLIC lwip_arch)
target_link_libraries(lwipallapps PUBLIC lwip_arch)