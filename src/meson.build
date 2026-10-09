luma_include = include_directories('.', '..')

core_sources = files(
  'suite/suite_identity.cpp',
  'suite/suite_context.cpp',
  'suite/suite_shell.cpp',
  'suite/suite_tokens.cpp',
  'commands/command_registry.cpp',
  'commands/command_catalog.cpp',
  'engine/engine_types.cpp',
  'engine/coordinate_space.cpp',
  'engine/uno_command_map.cpp',
  'engine/unavailable_engine.cpp',
  'engine/engine_factory.cpp',
  'canvas/tile_cache.cpp',
  'accessibility/accessibility_model.cpp',
  'accessibility/keyboard_traversal.cpp',
  'compatibility/format_policy.cpp',
  'compatibility/mvp_file_policy.cpp',
  'compatibility/document_state.cpp',
  'compatibility/save_advisor.cpp',
  'persistence/atomic_save.cpp',
  'persistence/recovery.cpp',
)

core_dependencies = [threads_dep]

if lok_enabled
  core_sources += files('engine/libreoffice_engine.cpp')
  dl_dep = cpp.find_library('dl', required: true)
  core_dependencies += [dl_dep]
endif

luma_write_core = static_library(
  'luma-write-core',
  core_sources,
  include_directories: luma_include,
  dependencies: core_dependencies,
  install: false,
)

luma_write_core_dep = declare_dependency(
  link_with: luma_write_core,
  include_directories: luma_include,
  dependencies: core_dependencies,
)

if desktop_enabled
  subdir('ui')
  subdir('app')

  luma_write_executable = executable(
    'luma-write',
    luma_write_ui_sources + luma_write_app_sources,
    include_directories: luma_include,
    dependencies: [
      luma_write_core_dep,
      gtk_dep,
      gtk_unix_print_dep,
      luma_appkit_dep,
      luma_appearance_dep,
      gio_unix_dep,
      fontconfig_dep,
      json_glib_dep,
      xml_dep,
    ],
    install: true,
  )
endif
