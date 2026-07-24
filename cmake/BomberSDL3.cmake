# Provides the SDL3::SDL3 target: from the system/vcpkg by default, or built
# from source via FetchContent when BOMBER_FETCH_SDL3=ON (no vcpkg needed).

if(BOMBER_FETCH_SDL3)
  include(FetchContent)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  # XTEST (X11 fake-input extension) is not used by the game, and its dev package
  # (libxtst-dev) is easy to miss on a fresh Linux/CI box — SDL's own configure
  # error suggests exactly this. Turning it off drops the libXtst dependency so a
  # from-source SDL3 build needs one fewer X11 package. X11-only: ignored on the
  # Windows/macOS builds (no X11 there), so this is safe everywhere.
  set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3)
else()
  find_package(SDL3 CONFIG REQUIRED)
endif()
