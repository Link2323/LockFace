add_executable(LockFaceService
    main.cpp
    FaceService.cpp
    auth_pipeline.cpp
    auth_frame_preparation.cpp
    auth_frame_stream.cpp
    authentication_residency.cpp
    auth_worker.cpp
    auth_worker_client.cpp
    auth_worker_protocol.cpp
    performance_affinity.cpp
    template_learner.cpp
    pipe_server.cpp
)

facelogin_add_version_rc(LockFaceService VFT_APP
    "LockFace authentication service" LockFaceService.exe)

target_link_libraries(LockFaceService PRIVATE
    facelogin_camera
    facelogin_inference
    facelogin_credential_store
    advapi32
    ole32
)

target_include_directories(LockFaceService PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
