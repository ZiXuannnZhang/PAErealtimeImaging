# The standard Windows kit ships libz.a and zlib.h in the MinGW sysroot.
# Use the compiler's own search paths rather than a workstation-specific path.
add_library(PAimageDiagnosticZlib INTERFACE)
if(MINGW)
    target_link_libraries(PAimageDiagnosticZlib INTERFACE z)
else()
    set(ZLIB_USE_STATIC_LIBS ON)
    find_package(ZLIB REQUIRED)
    target_link_libraries(PAimageDiagnosticZlib INTERFACE ZLIB::ZLIB)
endif()
