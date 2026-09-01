foreach(required_variable
        INPUT_HTML INPUT_CSS INPUT_JS INPUT_EXPOSURE_HTML INPUT_EXPOSURE_JS
        INPUT_RC INPUT_ICON PANO_APP_VERSION PANO_APP_VERSION_MAJOR
        PANO_APP_VERSION_MINOR PANO_APP_VERSION_PATCH OUTPUT_HTML
        OUTPUT_EXPOSURE_HTML OUTPUT_RC)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()
if(NOT EXISTS "${INPUT_ICON}")
    message(FATAL_ERROR "WebView application icon is missing: ${INPUT_ICON}")
endif()

file(READ "${INPUT_HTML}" html)
file(READ "${INPUT_CSS}" css)
file(READ "${INPUT_JS}" javascript)
file(READ "${INPUT_EXPOSURE_HTML}" exposure_html)
file(READ "${INPUT_EXPOSURE_JS}" exposure_javascript)
file(READ "${INPUT_RC}" resource_script)

set(stylesheet_reference
    "  <link rel=\"stylesheet\" href=\"pano_app_ui.css\">")
set(script_reference "  <script src=\"pano_app_ui.js\"></script>")
string(FIND "${html}" "${stylesheet_reference}" stylesheet_offset)
string(FIND "${html}" "${script_reference}" script_offset)
if(stylesheet_offset EQUAL -1 OR script_offset EQUAL -1)
    message(FATAL_ERROR "WebView UI source references are missing")
endif()

string(REPLACE "${stylesheet_reference}"
       "  <style id=\"_style\">${css}\n  </style>" embedded_html "${html}")
string(REPLACE "${script_reference}"
       "  <script>\n${javascript}\n  </script>" embedded_html "${embedded_html}")
set(exposure_script_reference
    "  <script src=\"pano_app_exposure.js\"></script>")
string(FIND "${exposure_html}" "${stylesheet_reference}" exposure_stylesheet_offset)
string(FIND "${exposure_html}" "${exposure_script_reference}" exposure_script_offset)
if(exposure_stylesheet_offset EQUAL -1 OR exposure_script_offset EQUAL -1)
    message(FATAL_ERROR "Exposure WebView UI source references are missing")
endif()
string(REPLACE "${stylesheet_reference}"
       "  <style id=\"_style\">${css}\n  </style>" embedded_exposure_html
       "${exposure_html}")
string(REPLACE "${exposure_script_reference}"
       "  <script>\n${exposure_javascript}\n  </script>"
       embedded_exposure_html "${embedded_exposure_html}")
file(TO_CMAKE_PATH "${INPUT_ICON}" resource_icon)
string(REPLACE "@PANO_APP_ICON@" "${resource_icon}"
       resource_script "${resource_script}")
foreach(version_variable
        PANO_APP_VERSION PANO_APP_VERSION_MAJOR PANO_APP_VERSION_MINOR
        PANO_APP_VERSION_PATCH)
    string(REPLACE "@${version_variable}@" "${${version_variable}}"
           resource_script "${resource_script}")
endforeach()

get_filename_component(output_directory "${OUTPUT_HTML}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT_HTML}" "${embedded_html}")
file(WRITE "${OUTPUT_EXPOSURE_HTML}" "${embedded_exposure_html}")
file(WRITE "${OUTPUT_RC}" "${resource_script}")
