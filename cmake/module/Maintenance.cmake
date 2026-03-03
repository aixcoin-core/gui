# Copyright (c) 2023-present The Aixcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

function(setup_split_debug_script)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(OBJCOPY ${CMAKE_OBJCOPY})
    set(STRIP ${CMAKE_STRIP})
    configure_file(
      contrib/devtools/split-debug.sh.in split-debug.sh
      FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ
      @ONLY
    )
  endif()
endfunction()

function(add_windows_deploy_target)
  if(MINGW AND TARGET aixcoin AND TARGET aixcoin-qt AND TARGET aixcoind AND TARGET aixcoin-cli AND TARGET aixcoin-tx AND TARGET aixcoin-wallet AND TARGET aixcoin-util AND TARGET test_aixcoin)
    find_program(MAKENSIS_EXECUTABLE makensis)
    if(NOT MAKENSIS_EXECUTABLE)
      add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E echo "Error: NSIS not found"
      )
      return()
    endif()

    # TODO: Consider replacing this code with the CPack NSIS Generator.
    #       See https://cmake.org/cmake/help/latest/cpack_gen/nsis.html
    include(GenerateSetupNsi)
    generate_setup_nsi()
    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/aixcoin-win64-setup.exe
      COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/release
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoin> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoin>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoin-qt> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoin-qt>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoind> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoind>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoin-cli> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoin-cli>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoin-tx> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoin-tx>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoin-wallet> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoin-wallet>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:aixcoin-util> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:aixcoin-util>
      COMMAND ${CMAKE_STRIP} $<TARGET_FILE:test_aixcoin> -o ${PROJECT_BINARY_DIR}/release/$<TARGET_FILE_NAME:test_aixcoin>
      COMMAND ${MAKENSIS_EXECUTABLE} -V2 ${PROJECT_BINARY_DIR}/aixcoin-win64-setup.nsi
      VERBATIM
    )
    add_custom_target(deploy DEPENDS ${PROJECT_BINARY_DIR}/aixcoin-win64-setup.exe)
  endif()
endfunction()

function(add_macos_deploy_target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET aixcoin-qt)
    set(macos_app "Aixcoin-Qt.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/aixcoin.icns ${macos_app}/Contents/Resources/aixcoin.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Aixcoin-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component aixcoin-qt --prefix ${macos_app}/Contents/MacOS --strip
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:aixcoin-qt> ${macos_app}/Contents/MacOS/Aixcoin-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/share
      VERBATIM
    )

    set(macos_zip "aixcoin-macos-app")
    if(CMAKE_HOST_APPLE)
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${macos_zip}.zip
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR} -zip=${macos_zip}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Aixcoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_zip}.zip
      )
      add_custom_target(deploy
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_zip}.zip
      )
    else()
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Aixcoin-Qt
        COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP} $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Aixcoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Aixcoin-Qt
      )

      find_program(ZIP_EXECUTABLE zip)
      if(NOT ZIP_EXECUTABLE)
        add_custom_target(deploy
          COMMAND ${CMAKE_COMMAND} -E echo "Error: ZIP not found"
        )
      else()
        add_custom_command(
          OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_zip}.zip
          WORKING_DIRECTORY dist
          COMMAND ${PROJECT_SOURCE_DIR}/cmake/script/macos_zip.sh ${ZIP_EXECUTABLE} ${macos_zip}.zip
          VERBATIM
        )
        add_custom_target(deploy
          DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_zip}.zip
        )
      endif()
    endif()
    add_dependencies(deploydir aixcoin-qt)
    add_dependencies(deploy deploydir)
  endif()
endfunction()
