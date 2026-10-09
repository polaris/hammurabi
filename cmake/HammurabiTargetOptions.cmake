# Per-target build settings for hammurabi's own code.
#
# Both functions set options PRIVATE, so they never leak to consumers of a target.
# Third-party and generated code is excluded by simply not calling them on those targets.

function(hammurabi_target_warnings target)
    target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            $<$<BOOL:${HAMMURABI_WARNINGS_AS_ERRORS}>:-Werror>)
endfunction()

# HAMMURABI_SANITIZERS is passed straight to -fsanitize=, e.g. "address,undefined" or "thread".
function(hammurabi_target_sanitizers target)
    if(NOT HAMMURABI_SANITIZERS)
        return()
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=${HAMMURABI_SANITIZERS} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=${HAMMURABI_SANITIZERS})
endfunction()

function(hammurabi_target_clang_tidy target)
    if(HAMMURABI_CLANG_TIDY)
        find_program(HAMMURABI_CLANG_TIDY_EXE clang-tidy REQUIRED)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${HAMMURABI_CLANG_TIDY_EXE}")
    endif()
endfunction()
