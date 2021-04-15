
# TODO: which components do I need?
find_package(PkgConfig REQUIRED)

pkg_search_module(GLIB REQUIRED glib-2.0)

pkg_search_module(GST REQUIRED     gstreamer-1.0 )
pkg_search_module(GSTAPP REQUIRED     gstreamer-app-1.0 )
#pkg_check_modules(GST REQUIRED   IMPORTED_TARGET  gstreamer-1.0)

MJPG_STREAMER_PLUGIN_OPTION(input_gstreamer "GStreamer input plugin")

if (PLUGIN_INPUT_GSTREAMER)
    add_definitions(-DLINUX -D_GNU_SOURCE)

        #include_directories(/usr/include/gstreamer-1.0)
    include_directories("${GST_INCLUDE_DIRS}" "${GSTAPP_INCLUDE_DIRS}")
    link_directories("${GST_LIBRARY_DIRS} ${GSTAPP_LIBRARY_DIRS}")

    #target_link_libraries(input_gstreamer PUBLIC        PkgConfig::${GST_LIBRARIES})

    MJPG_STREAMER_PLUGIN_COMPILE(input_gstreamer input_gstreamer.c)
    target_link_libraries(input_gstreamer ${GST_LIBRARIES} ${GSTAPP_LIBRARIES})

endif()


