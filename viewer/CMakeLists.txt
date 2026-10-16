if(CONFIG_ZMK_KEYBOARD_VIEWER)
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(viewer.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_KEYBOARD_VIEWER_BLE viewer_ble.c)
  zephyr_link_libraries(-Wl,--wrap=raise_zmk_position_state_changed)
  zephyr_link_libraries(-Wl,--wrap=zmk_behavior_invoke_binding)
  zephyr_link_libraries(-Wl,--wrap=zmk_keymap_set_layer_binding_at_idx)
  foreach(symbol zmk_keymap_discard_changes zmk_keymap_reset_settings)
    zephyr_link_libraries(-Wl,--wrap=${symbol})
  endforeach()
  if(CONFIG_ZMK_KEYMAP_LAYER_REORDERING)
    foreach(symbol zmk_keymap_set_layer_name zmk_keymap_add_layer zmk_keymap_remove_layer
        zmk_keymap_restore_layer zmk_keymap_move_layer)
      zephyr_link_libraries(-Wl,--wrap=${symbol})
    endforeach()
  endif()
endif()

if(CONFIG_ZMK_RUNTIME_FACTORY)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/runtime_safety.c)
  function(runtime_factory_patch_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/patch_runtime.py)
    get_target_property(app_sources app SOURCES)
    set(patch_count 0)
    foreach(source IN LISTS app_sources)
      if(source MATCHES "(^|/)src/(studio/runtime_combo_handler|runtime_macro(_dt_defaults)?|custom_settings(_keyspace)?|behavior)\\.c$")
        set(original_source ${source})
        if(NOT IS_ABSOLUTE "${original_source}")
          set(original_source ${APPLICATION_SOURCE_DIR}/${source})
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${original_source})
        get_filename_component(filename ${source} NAME)
        set(patched ${CMAKE_BINARY_DIR}/runtime-factory/${filename})
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/runtime-factory)
        get_filename_component(original_dir ${original_source} DIRECTORY)
        target_include_directories(app PRIVATE ${original_dir})
        execute_process(
          COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/patch_runtime.py
            ${original_source} ${patched}
          RESULT_VARIABLE patch_result)
        if(NOT patch_result EQUAL 0)
          message(FATAL_ERROR "Pinned runtime adaptation failed")
        endif()
        list(REMOVE_ITEM app_sources ${source})
        list(APPEND app_sources ${patched})
        math(EXPR patch_count "${patch_count} + 1")
      endif()
    endforeach()
    if(NOT patch_count EQUAL 6)
      message(FATAL_ERROR "Expected all six pinned runtime sources before adaptation")
    endif()
    set_property(TARGET app PROPERTY SOURCES "${app_sources}")
  endfunction()
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL runtime_factory_patch_sources)
  foreach(symbol
      zmk_runtime_combo_write zmk_runtime_combo_delete zmk_runtime_combo_reset
      zmk_runtime_combo_write_name zmk_runtime_combo_write_timeout_ms
      zmk_runtime_combo_write_slow_release zmk_runtime_combo_write_require_prior_idle_ms
      zmk_runtime_macro_create zmk_runtime_macro_write zmk_runtime_macro_delete
      zmk_runtime_macro_rename zmk_custom_setting_write_bytes
      zmk_custom_setting_write_array_element zmk_custom_setting_write
      zmk_custom_setting_reset zmk_custom_setting_discard
      zmk_custom_settings_reset_scope zmk_custom_settings_discard_scope
      zmk_behavior_queue_add
      zmk_custom_setting_keyspace_create zmk_custom_setting_keyspace_delete)
    zephyr_link_libraries(-Wl,--wrap=${symbol})
  endforeach()
  zephyr_link_libraries(-Wl,--wrap=raise_zmk_position_state_changed)
endif()

if(CONFIG_ZMK_MOUSE_SPEED)
  zephyr_library_named(mouse_speed)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(../mouse/mouse_speed.c)
endif()
