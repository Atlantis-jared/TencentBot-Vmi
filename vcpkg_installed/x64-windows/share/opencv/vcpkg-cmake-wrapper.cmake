set(OpenCV_DIR "${CMAKE_CURRENT_LIST_DIR}/../opencv4/" CACHE PATH "Path to OpenCVConfig.cmake" FORCE)
set(OpenCV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../opencv4/")
_find_package(${ARGS})
