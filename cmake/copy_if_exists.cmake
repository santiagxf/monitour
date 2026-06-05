# Copies SRC to DST only if SRC exists. Used to copy optional ONNX models next
# to the executable without failing the build when they are absent.
if(EXISTS "${SRC}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}")
else()
    message(STATUS "Optional file not found, skipping copy: ${SRC}")
endif()
