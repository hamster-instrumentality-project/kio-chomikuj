kcoreaddons_add_plugin(kcm_chomikuj
    SOURCES
        chomikujkcm.cpp
        chomikujkcm.h
        kcm_chomikuj.json
    INSTALL_NAMESPACE plasma/kcms/systemsettings_qwidgets
)

target_link_libraries(kcm_chomikuj
    Qt6::Widgets
    KF6::KCMUtils
    KF6::ConfigCore
    KF6::I18n
)

set_target_properties(kcm_chomikuj PROPERTIES OUTPUT_NAME "kcm_chomikuj")

kcmutils_generate_desktop_file(kcm_chomikuj)
