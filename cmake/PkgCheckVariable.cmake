find_package(PkgConfig REQUIRED)

function(pkg_check_variable VARIABLE  _pkg _name)
  execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=${_name} ${_pkg}
	OUTPUT_VARIABLE _pkg_result
	OUTPUT_STRIP_TRAILING_WHITESPACE)

  set("${VARIABLE}" "${_pkg_result}" CACHE STRING "pkg-config variable ${_name} of ${_pkg}")
endfunction()
