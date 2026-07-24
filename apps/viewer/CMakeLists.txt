add_executable(bomber_viewer main.cpp)
target_link_libraries(bomber_viewer PRIVATE bomber::game bomber::warnings)

# Windows: copy any runtime DLLs next to the exe (see apps/game/CMakeLists.txt).
# With SDL3 linked statically TARGET_RUNTIME_DLLS is empty, so the $<IF:...>
# swaps in `cmake -E true` and copy_if_different never runs with no source.
if(WIN32)
  add_custom_command(TARGET bomber_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E
            $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:bomber_viewer>>,copy_if_different,true>
            $<TARGET_RUNTIME_DLLS:bomber_viewer> $<TARGET_FILE_DIR:bomber_viewer>
    COMMAND_EXPAND_LISTS)
endif()
