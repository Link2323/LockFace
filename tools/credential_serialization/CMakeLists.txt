set(CREDENTIAL_TEST_SOURCES
    ../../credential_provider/native_logon.cpp
    ../../credential_provider/LockFaceCredential.cpp
    ../../credential_provider/LockFaceProvider.cpp
    ../../credential_provider/pipe_client.cpp
    ../../credential_provider/dllmain.cpp)
foreach(test_target CredentialSerializationTest CredentialDomainRoutingTest)
    if(test_target STREQUAL "CredentialSerializationTest")
        set(test_source credential_serialization_test.cpp)
    else()
        set(test_source domain_routing_test.cpp)
    endif()
    add_executable(${test_target} ${test_source} ${CREDENTIAL_TEST_SOURCES})
    target_compile_definitions(${test_target} PRIVATE FACELOGIN_SERIALIZATION_TESTS)
    target_link_libraries(${test_target} PRIVATE facelogin_common credui advapi32 ole32 oleaut32 kernel32 shlwapi netapi32 secur32)
endforeach()

add_executable(CredentialConcurrencyTest credential_concurrency_test.cpp ${CREDENTIAL_TEST_SOURCES})
target_compile_definitions(CredentialConcurrencyTest PRIVATE FACELOGIN_SERIALIZATION_TESTS FACELOGIN_CONCURRENCY_TESTS)
target_link_libraries(CredentialConcurrencyTest PRIVATE facelogin_common credui advapi32 ole32 oleaut32 kernel32 shlwapi netapi32 secur32)
