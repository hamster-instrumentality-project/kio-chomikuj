function(add_chomikuj_test name)
    add_executable(${name} ${name}.cpp faketransport.h)
    target_link_libraries(${name} PRIVATE Qt6::Test chomikujcore)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_chomikuj_test(tst_signing)
add_chomikuj_test(tst_client)
add_chomikuj_test(tst_upload)
add_chomikuj_test(tst_filesystem)
add_chomikuj_test(tst_plugin)

set_tests_properties(tst_plugin PROPERTIES ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_BINARY_DIR}/bin")

add_executable(chomikuj_live live_check.cpp)
target_link_libraries(chomikuj_live PRIVATE chomikujcore)
add_test(NAME chomikuj_live COMMAND chomikuj_live)
set_tests_properties(chomikuj_live PROPERTIES TIMEOUT 300)
