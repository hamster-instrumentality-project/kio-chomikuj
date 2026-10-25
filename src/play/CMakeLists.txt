add_executable(kio-chomikuj-play
    chomikujplay.cpp
    playurl.cpp
    playurl.h
)
target_include_directories(kio-chomikuj-play PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kio-chomikuj-play
    PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Widgets
        KF6::ConfigCore
)

install(TARGETS kio-chomikuj-play ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES kio-chomikuj-play.desktop
        DESTINATION ${KDE_INSTALL_DATADIR}/applications)
install(FILES chomikuj-play.desktop
        DESTINATION ${KDE_INSTALL_DATADIR}/kio/servicemenus)
