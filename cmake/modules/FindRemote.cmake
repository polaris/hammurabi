# FindRemote
# --------
#
# Find libremote
#
# Result Variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ``REMOTE_FOUND``
#   True if REMOTE_INCLUDE_DIR & REMOTE_LIBRARY are found
#
# ``REMOTE_LIBRARIES``
#   List of libraries when using REMOTE.
#
# ``REMOTE_INCLUDE_DIRS``
#   Where to find the REMOTE headers.
#
# Cache variables
# ^^^^^^^^^^^^^^^
#
# The following cache variables may also be set:
#
# ``REMOTE_INCLUDE_DIR``
#   the remote include directory
#
# ``REMOTE_LIBRARY``
#   the absolute path of the remote library

find_path(REMOTE_INCLUDE_DIR NAMES remote/rpc_client.h
        DOC "The libremote include directory")

find_library(REMOTE_LIBRARY NAMES remote
        DOC "The libremote")

set(REMOTE_VERSION_STRING 1.0.0)

if(REMOTE_FOUND)
    set(REMOTE_LIBRARIES ${REMOTE_LIBRARY})
    set(REMOTE_INCLUDE_DIRS ${REMOTE_INCLUDE_DIR})
    if(NOT TARGET Remote::Remote)
        add_library(Remote::Remote UNKNOWN IMPORTED)
        set_target_properties(Remote::Remote PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${REMOTE_INCLUDE_DIRS}")
        set_property(TARGET Remote::Remote APPEND PROPERTY IMPORTED_LOCATION "${REMOTE_LIBRARIES}")
    endif()
endif()

find_package_handle_standard_args(Remote
        REQUIRED_VARS REMOTE_LIBRARY REMOTE_INCLUDE_DIR
        VERSION_VAR REMOTE_VERSION_STRING)

mark_as_advanced(REMOTE_INCLUDE_DIR REMOTE_LIBRARY)
