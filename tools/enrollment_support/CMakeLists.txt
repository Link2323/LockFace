add_executable(EnrollmentSupportTest enrollment_support_test.cpp
    ../../face_service/pipe_server.cpp)
target_link_libraries(EnrollmentSupportTest PRIVATE facelogin_enrollment_support)
