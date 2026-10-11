test_specs = [
  ['suite-contract', 'suite_contract_test.cpp'],
  ['command-registry', 'command_registry_test.cpp'],
  ['engine-core', 'engine_core_test.cpp'],
  ['office-locations', 'office_locations_test.cpp'],
  ['accessibility-model', 'accessibility_model_test.cpp'],
  ['accessibility-traversal', 'accessibility_traversal_test.cpp'],
  ['compatibility-policy', 'compatibility_policy_test.cpp'],
  ['mvp-file-policy', 'mvp_file_policy_test.cpp'],
  ['persistence-atomic-save', 'persistence_atomic_save_test.cpp'],
  ['persistence-recovery', 'persistence_recovery_test.cpp'],
]

foreach spec : test_specs
  test_executable = executable(
    spec[0],
    spec[1],
    include_directories: luma_include,
    dependencies: [luma_write_core_dep],
  )
  test(
    spec[0],
    test_executable,
    timeout: 60,
    suite: ['unit'],
  )
endforeach

if lok_enabled
  libreoffice_engine_integration = executable(
    'libreoffice-engine-integration',
    'libreoffice_engine_integration_test.cpp',
    include_directories: luma_include,
    dependencies: [luma_write_core_dep, dependency('glib-2.0')],
  )
  test(
    'libreoffice-engine-integration',
    libreoffice_engine_integration,
    env: [
      'SAL_USE_VCLPLUGIN=svp',
      'LUMA_LIBREOFFICE_PROGRAM=/usr/lib64/libreoffice/program',
    ],
    timeout: 180,
    is_parallel: false,
    suite: ['integration'],
  )
  foreach format : ['odt', 'doc', 'pdf', 'header-footer', 'format-panel', 'images']
    test(
      'libreoffice-engine-export-' + format,
      libreoffice_engine_integration,
      args: [format],
      env: [
        'SAL_USE_VCLPLUGIN=svp',
        'LUMA_LIBREOFFICE_PROGRAM=/usr/lib64/libreoffice/program',
      ],
      timeout: 180,
      is_parallel: false,
      suite: ['integration'],
    )
  endforeach

  native_ui_smoke_runner = find_program(
    meson.project_source_root() / 'scripts' / 'run-native-ui-smoke.sh',
  )
  test(
    'native-ui-smoke',
    native_ui_smoke_runner,
    args: [
      luma_write_executable,
      meson.current_build_dir() / 'native-ui-smoke-artifacts',
    ],
    depends: [luma_write_executable],
    is_parallel: false,
    timeout: 180,
    suite: ['integration', 'desktop'],
  )
endif

if desktop_enabled
  ui_responsive_test = executable(
    'ui-responsive',
    ['ui_responsive_test.cpp'] + luma_write_ui_sources,
    include_directories: luma_include,
    dependencies: [luma_write_core_dep, gtk_dep, luma_appkit_dep, gio_unix_dep, fontconfig_dep, json_glib_dep, xml_dep],
  )
  test('ui-responsive', ui_responsive_test,
    timeout: 90, is_parallel: false, suite: ['desktop'],
  )
endif
