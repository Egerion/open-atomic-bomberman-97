add_executable(bomber_viewer main.cpp)
# bomber::render, not bomber::game: the viewer opens its own window and draws
# ANI banks (AniTextures), so the renderer package is all it needs. It used to
# link the whole presentation layer because there was only one target to link;
# the split makes the real dependency visible, and now a change to a screen or
# to the lobby cannot rebuild or break this tool.
target_link_libraries(bomber_viewer PRIVATE bomber::render bomber::warnings)

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
