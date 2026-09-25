add_executable(share)

target_sources(share PUBLIC
    main.cpp
)

target_include_directories(share PUBLIC .)

if (WIN32)
    target_compile_definitions(share PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_link_libraries(share PRIVATE ws2_32 iphlpapi shell32)
endif()
if (MSVC)
    target_compile_options(share PRIVATE /utf-8)
endif()
if (MINGW)
    # A self-contained .exe, without the MinGW runtime DLLs next to it.
    target_link_options(share PRIVATE -static)
endif()

install(TARGETS share)
