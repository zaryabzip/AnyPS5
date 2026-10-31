add_library(libSceAgcVsh SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceAgcVsh PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(libSceAgcVsh PRIVATE libSceAgc)
configure_windows_unwind(libSceAgcVsh)

