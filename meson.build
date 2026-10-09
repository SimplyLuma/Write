project(
  'luma-write',
  'cpp',
  version: '0.1.0',
  meson_version: '>=1.4.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'werror=true',
    'buildtype=debugoptimized',
  ],
)

subdir('identity')

cpp = meson.get_compiler('cpp')
threads_dep = dependency('threads')

add_project_arguments(
  cpp.get_supported_arguments([
    '-Wconversion',
    '-Wshadow',
    '-Wnon-virtual-dtor',
    '-Woverloaded-virtual',
    '-Wpedantic',
    # C++20 designated initializers intentionally rely on member defaults for
    # fields that are not relevant to a particular event/result variant.
    '-Wno-missing-field-initializers',
  ]),
  language: 'cpp',
)

sanitizers = get_option('sanitizers')
if sanitizers != 'none'
  sanitizer_args = ['-fsanitize=' + sanitizers, '-fno-omit-frame-pointer']
  add_project_arguments(sanitizer_args, language: 'cpp')
  add_project_link_arguments(sanitizer_args, language: 'cpp')
endif

desktop_enabled = not get_option('desktop').disabled()
if desktop_enabled
  gtk_dep = dependency('gtk4', version: '>=4.22', required: get_option('desktop'))
  gtk_unix_print_dep = dependency('gtk4-unix-print', required: get_option('desktop'))
  luma_appearance_dep = dependency('luma-appearance-1', required: get_option('desktop'))
  luma_appkit_dep = dependency('luma-ui-1', version: '>=0.1.0',
    required: get_option('desktop'))
  gio_unix_dep = dependency('gio-unix-2.0', version: '>=2.80', required: get_option('desktop'))
  fontconfig_dep = dependency('fontconfig', required: get_option('desktop'))
  json_glib_dep = dependency('json-glib-1.0', required: get_option('desktop'))
  xml_dep = dependency('libxml-2.0', required: get_option('desktop'))
  desktop_enabled = gtk_dep.found() and luma_appkit_dep.found() and gio_unix_dep.found() and fontconfig_dep.found() and json_glib_dep.found() and xml_dep.found()
endif

lok_enabled = desktop_enabled and not get_option('libreofficekit').disabled()
if lok_enabled
  lok_header_found = cpp.has_header(
    'LibreOfficeKit/LibreOfficeKit.hxx',
    required: get_option('libreofficekit'),
  )
  lok_enabled = lok_header_found
endif

config = configuration_data()
config.set_quoted('LUMA_SUITE_NAME', luma_suite_name)
config.set_quoted('LUMA_APP_NAME', luma_app_name)
config.set_quoted('LUMA_APP_ID', luma_app_id)
config.set_quoted('LUMA_ICON_NAME', luma_icon_name)
config.set_quoted('LUMA_DOCX_LABEL', luma_docx_label)
config.set_quoted('LUMA_ODT_LABEL', luma_odt_label)
config.set_quoted('LUMA_DOC_LABEL', luma_doc_label)
config.set_quoted('LUMA_PDF_LABEL', luma_pdf_label)
config.set_quoted('LUMA_VERSION', meson.project_version())
config.set10('LUMA_HAS_DESKTOP', desktop_enabled)
config.set10('LUMA_HAS_LIBREOFFICEKIT', lok_enabled)
configure_file(
  input: 'src/luma_config.hpp.in',
  output: 'luma_config.hpp',
  configuration: config,
)

subdir('src')
subdir('tests')
if desktop_enabled
  subdir('resources')
  subdir('data')
endif

summary({
  'Application': luma_app_name,
  'Application ID': luma_app_id,
  'Desktop application': desktop_enabled,
  'LibreOfficeKit adapter': lok_enabled,
  'Developer tools': get_option('developer_tools'),
  'Sanitizers': sanitizers,
}, section: 'Luma Write')
