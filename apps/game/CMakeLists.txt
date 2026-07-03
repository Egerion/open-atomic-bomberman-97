add_executable(bomber_game main.cpp)
target_link_libraries(bomber_game PRIVATE bomber::game bomber::warnings)

# Windows: copy SDL3.dll (and any other runtime DLLs) next to the exe,
# otherwise it dies with STATUS_DLL_NOT_FOUND outside the IDE.
if(WIN32)
  add_custom_command(TARGET bomber_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:bomber_game> $<TARGET_FILE_DIR:bomber_game>
    COMMAND_EXPAND_LISTS)
endif()
