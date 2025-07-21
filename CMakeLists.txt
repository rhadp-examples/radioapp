project(auto-apps)

cmake_minimum_required (VERSION 2.10)

include(GNUInstallDirs)

find_package (vsomeip3 2.6.0 REQUIRED)
find_package( Boost 1.55 COMPONENTS system thread log REQUIRED )

set (INSTALL_LIB_DIR ${CMAKE_INSTALL_LIBDIR} CACHE PATH "Installation directory for libraries")
set (INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR} CACHE PATH "Installation directory for executables")
set (INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR} CACHE PATH "Installation directory for header files")

include_directories (
    ${Boost_INCLUDE_DIR}
    ${VSOMEIP_INCLUDE_DIRS}
)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(radio-service radio-service.cpp services.hpp radio-stations.cpp)
target_link_libraries(radio-service  vsomeip3 ${Boost_LIBRARIES})

add_executable(radio-client radio-client.cpp services.hpp)
target_link_libraries(radio-client  vsomeip3 ${Boost_LIBRARIES})

add_executable(engine-service engine-service.cpp services.hpp)
target_link_libraries(engine-service  vsomeip3 ${Boost_LIBRARIES})

install (
    TARGETS radio-service radio-client engine-service
    RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin
)
