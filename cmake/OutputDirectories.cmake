include_guard(GLOBAL)

set(SIMBEHAVIOR_OUTPUT_ROOT
    "${CMAKE_BINARY_DIR}"
    CACHE PATH "Root directory for all built artifacts")

if(CMAKE_CONFIGURATION_TYPES)
  set(_simbehavior_default_output_dir "${SIMBEHAVIOR_OUTPUT_ROOT}")
else()
  if(CMAKE_BUILD_TYPE)
    set(_simbehavior_default_output_dir
        "${SIMBEHAVIOR_OUTPUT_ROOT}/${CMAKE_BUILD_TYPE}")
  else()
    set(_simbehavior_default_output_dir "${SIMBEHAVIOR_OUTPUT_ROOT}")
  endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${_simbehavior_default_output_dir}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${_simbehavior_default_output_dir}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${_simbehavior_default_output_dir}")

if(MSVC)
  set(CMAKE_PDB_OUTPUT_DIRECTORY "${_simbehavior_default_output_dir}")
  set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${_simbehavior_default_output_dir}")
endif()

foreach(_simbehavior_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_simbehavior_cfg}" _simbehavior_cfg_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
      "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
      "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
      "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}")

  if(MSVC)
    set(CMAKE_PDB_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
        "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}")
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
        "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}")
  endif()
endforeach()

function(simbehavior_set_target_output_directories target_name)
  if(NOT TARGET "${target_name}")
    return()
  endif()

  set_target_properties("${target_name}" PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
  )

  if(MSVC)
    set_target_properties("${target_name}" PROPERTIES
      PDB_OUTPUT_DIRECTORY "${CMAKE_PDB_OUTPUT_DIRECTORY}"
      COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY}"
    )
  endif()

  foreach(_simbehavior_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_simbehavior_cfg}" _simbehavior_cfg_upper)
    set_target_properties("${target_name}" PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
        "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}"
      LIBRARY_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
        "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}"
      ARCHIVE_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
        "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}"
    )

    if(MSVC)
      set_target_properties("${target_name}" PROPERTIES
        PDB_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
          "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}"
        COMPILE_PDB_OUTPUT_DIRECTORY_${_simbehavior_cfg_upper}
          "${SIMBEHAVIOR_OUTPUT_ROOT}/${_simbehavior_cfg}"
      )
    endif()
  endforeach()
endfunction()

unset(_simbehavior_default_output_dir)
unset(_simbehavior_cfg)
unset(_simbehavior_cfg_upper)
