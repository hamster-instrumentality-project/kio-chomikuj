function(add_chomikuj_test name)
    add_executable(${name} ${name}.cpp faketransport.h)
    target_link_libraries(${name} PRIVATE Qt6::Test chomikujcore)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_chomikuj_test(tst_signing)
add_chomikuj_test(tst_client)
add_chomikuj_test(tst_upload)
add_chomikuj_test(tst_filesystem)
add_chomikuj_test(tst_range)
add_chomikuj_test(tst_plugin)

add_executable(tst_play tst_play.cpp ${CMAKE_SOURCE_DIR}/src/play/playurl.cpp)
target_include_directories(tst_play PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_play PRIVATE Qt6::Test)
add_test(NAME tst_play COMMAND tst_play)

add_test(NAME kio_chomikuj_play_url
         COMMAND kio-chomikuj-play --print-url chomikuj://account/movie.mkv)
set_tests_properties(kio_chomikuj_play_url PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    PASS_REGULAR_EXPRESSION "chomikujstream://account/movie.mkv")

set_tests_properties(tst_plugin PROPERTIES ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_BINARY_DIR}/bin")

add_executable(kio_open_check kio_open_check.cpp)
target_link_libraries(kio_open_check PRIVATE Qt6::Test Qt6::Network KF6::KIOCore)
add_test(NAME kio_open_check COMMAND kio_open_check)
set_tests_properties(kio_open_check PROPERTIES
    ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_BINARY_DIR}/bin"
    TIMEOUT 300)

add_executable(chomikuj_live live_check.cpp)
target_link_libraries(chomikuj_live PRIVATE chomikujcore)
add_test(NAME chomikuj_live COMMAND chomikuj_live)
set_tests_properties(chomikuj_live PROPERTIES TIMEOUT 300)
