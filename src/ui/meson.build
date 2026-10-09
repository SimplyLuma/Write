glib_compile_resources = find_program('glib-compile-resources')
luma_write_resource_root = join_paths(meson.current_source_dir(), '../../resources')

# Generate C++ because the project intentionally enables only the C++ compiler.
luma_write_ui_resources = custom_target(
  'luma-write-ui-resources',
  input: '../../resources/luma-write.gresource.xml',
  output: 'luma-write-ui-resources.cpp',
  depfile: 'luma-write-ui-resources.d',
  command: [
    glib_compile_resources,
    '--generate-source',
    '--c-name=luma_write_ui',
    '--dependency-file=@DEPFILE@',
    '--sourcedir=' + luma_write_resource_root,
    '--target=@OUTPUT@',
    '@INPUT@',
  ],
  depend_files: files(
    '../../resources/css/write.css',
    '../../lumaui/fixtures/life-professionals-loft.png',
  ),
)

luma_write_ui_sources = files(
  'write_window.cpp',
  'write_panels.cpp',
  'write_combo.cpp',
  '../canvas/document_canvas.cpp',
  '../canvas/document_canvas_images.cpp',
) + [luma_write_ui_resources]
luma_write_ui_headers = files('write_window.hpp')
