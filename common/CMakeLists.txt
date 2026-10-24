add_library(facelogin_common STATIC
    account_identity.cpp
    ipc_protocol.cpp
    dpapi_util.cpp
    logger.cpp
    config_util.cpp
    sha256_util.cpp
    data_path.cpp
    acl_util.cpp
)

target_include_directories(facelogin_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(facelogin_common PUBLIC
    advapi32
    kernel32
    crypt32
)
