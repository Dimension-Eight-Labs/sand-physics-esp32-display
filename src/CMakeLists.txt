FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
idf_component_register(SRCS ${app_sources} INCLUDE_DIRS ".")
# Extra warnings for our own code only.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wdouble-promotion -Wshadow)
