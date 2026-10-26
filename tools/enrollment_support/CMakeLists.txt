add_executable(EnrollmentSupportTest enrollment_support_test.cpp
    ../enrollment_ui/preview_frame_encoder_test.cpp
    ../../enrollment_app/preview_frame_encoder.cpp
    ../../face_service/pipe_server.cpp)
target_link_libraries(EnrollmentSupportTest PRIVATE facelogin_enrollment_support
    windowscodecs ole32 oleaut32 crypt32 shlwapi)
