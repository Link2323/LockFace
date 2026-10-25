# Development-only DirectShow lifecycle probe. This target is never shipped.
add_executable(CameraLifecycleTest
    camera_lifecycle.cpp
)

target_link_libraries(CameraLifecycleTest PRIVATE
    facelogin_camera
    psapi
)

add_executable(ExposureResponseProbe exposure_response_probe.cpp)
target_link_libraries(ExposureResponseProbe PRIVATE facelogin_camera)
set_target_properties(ExposureResponseProbe PROPERTIES
    EXCLUDE_FROM_ALL TRUE
)

# Read-only comparison of the first control query and camera startup.
add_executable(CameraStartupProbe EXCLUDE_FROM_ALL camera_startup_probe.cpp)
target_link_libraries(CameraStartupProbe PRIVATE facelogin_camera winmm)
