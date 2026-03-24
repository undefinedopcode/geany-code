project('geany-code', 'c',
  version: '0.1.0',
  meson_version: '>= 0.60',
  default_options: ['c_std=c11', 'warning_level=2'],
)

geany_dep = dependency('geany', version: '>= 2.0')
gtk_dep = dependency('gtk+-3.0')
webkit_dep = dependency('webkit2gtk-4.1')
json_dep = dependency('json-glib-1.0')

plugin_sources = files(
  'src/plugin.c',
  'src/chat_widget.c',
  'src/chat_webview.c',
  'src/chat_input.c',
  'src/cli_session.c',
  'src/editor_bridge.c',
  'src/session_picker.c',
  'src/settings.c',
)

# Install web assets to plugin data directory
install_data(
  'data/web/chat.html',
  'data/web/chat.css',
  'data/web/chat.js',
  install_dir: get_option('datadir') / 'geany-code' / 'web',
)

# Install vendor JS libraries
install_subdir('data/web/vendor',
  install_dir: get_option('datadir') / 'geany-code' / 'web',
)

# Compile web asset paths into plugin
data_dir = get_option('prefix') / get_option('datadir') / 'geany-code'

shared_library('geany-code',
  plugin_sources,
  dependencies: [geany_dep, gtk_dep, webkit_dep, json_dep],
  c_args: [
    '-DGEANY_CODE_DATA_DIR="@0@"'.format(data_dir),
    '-DGEANY_CODE_VERSION="@0@"'.format(meson.project_version()),
  ],
  install: true,
  install_dir: geany_dep.get_variable(pkgconfig: 'libdir') / 'geany',
  name_prefix: '',  # produce geany-code.so, not libgeany-code.so
)
