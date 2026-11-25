# pcsx2-dsrtest: headless PCSX2 test runner. Derived from PCSX2 v2.8.2 pcsx2-gsrunner/CMakeLists.txt (GPLv3);
# tools/pcsx2-headless/build.sh copies this file into <pcsx2-src>/pcsx2-dsrtest/.
add_executable(pcsx2-dsrtest)

target_sources(pcsx2-dsrtest PRIVATE
	Main.cpp
)

target_include_directories(pcsx2-dsrtest PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-dsrtest PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# bin/resources next to the binary, as setup_main_executable (pcsx2/CMakeLists.txt:1271-1283) does for pcsx2-qt.
# That function itself is not called: on Linux it also installs the pcsx2-qt target, which a no-Qt build lacks.
file(GLOB_RECURSE DSRTEST_RESOURCES ${CMAKE_SOURCE_DIR}/bin/resources/*)
foreach(path IN LISTS DSRTEST_RESOURCES)
	get_filename_component(file ${path} NAME)
	if("${file}" MATCHES "^\\." OR "${path}" MATCHES "/dx11/" OR "${path}" MATCHES "fonts/Twemoji")
		continue()
	endif()
	pcsx2_resource(pcsx2-dsrtest ${path} ${CMAKE_SOURCE_DIR}/bin/resources/)
endforeach()
