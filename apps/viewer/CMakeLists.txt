add_executable(bomber_viewer main.cpp)
target_link_libraries(bomber_viewer PRIVATE bomber::game bomber::warnings)

# Windows: copy SDL3.dll next to the exe (see apps/game/CMakeLists.txt).
if(WIN32)
  add_custom_command(TARGET bomber_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:bomber_viewer> $<TARGET_FILE_DIR:bomber_viewer>
    COMMAND_EXPAND_LISTS)
endif()
