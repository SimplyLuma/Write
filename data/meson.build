luma_settings_path = '/' + luma_app_id.to_lower().replace('.', '/') + '/'

desktop_file = configure_file(
  input: 'org.projectluma.Write.desktop.in',
  output: luma_app_id + '.desktop',
  configuration: {
    'APP_NAME': luma_app_name,
    'APP_ID': luma_app_id,
  },
  install: true,
  install_dir: get_option('datadir') / 'applications',
)

appstream_file = configure_file(
  input: 'org.projectluma.Write.metainfo.xml.in',
  output: luma_app_id + '.metainfo.xml',
  configuration: {
    'APP_NAME': luma_app_name,
    'APP_ID': luma_app_id,
    'VERSION': meson.project_version(),
  },
  install: true,
  install_dir: get_option('datadir') / 'metainfo',
)

mime_file = configure_file(
  input: 'org.projectluma.Write.mime.xml.in',
  output: luma_app_id + '.xml',
  configuration: {
    'DOCX_LABEL': luma_docx_label,
    'ODT_LABEL': luma_odt_label,
    'DOC_LABEL': luma_doc_label,
  },
  install: true,
  install_dir: get_option('datadir') / 'mime/packages',
)

settings_file = configure_file(
  input: 'org.projectluma.Write.gschema.xml.in',
  output: luma_app_id + '.gschema.xml',
  configuration: {
    'APP_ID': luma_app_id,
    'SETTINGS_PATH': luma_settings_path,
  },
  install: true,
  install_dir: get_option('datadir') / 'glib-2.0/schemas',
)

install_data(
  'org.projectluma.Write.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
  rename: luma_app_id + '.svg',
)

install_data(
  'org.projectluma.Write-symbolic.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/symbolic/apps',
  rename: luma_app_id + '-symbolic.svg',
)

gnome = import('gnome')
gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
  update_mime_database: true,
)
