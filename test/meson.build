test_option_parser = executable(
  'test-option-parser',
  ['..' / 'src' / 'celluloid-option-parser.c', 'test-option-parser.c'],
  include_directories: include_directories('..' / 'src'),
  dependencies: libgtk
)

test_playlist_model = executable(
  'test-playlist-model',
  generated_marshal_sources +
  [ '..' / 'src' / 'celluloid-playlist-model.c',
    '..' / 'src' / 'celluloid-playlist-item.c',
    'test-playlist-model.c'],
  include_directories: include_directories('..' / 'src'),
  dependencies: libgtk
)

test('test-option-parser', test_option_parser)
test('test-playlist-model', test_playlist_model)

# The test includes the widget implementation to check private async lifecycle
# state; --gc-sections retains only common helpers needed by these widgets.
preview_test = executable('test-seek-preview',
  ['test-seek-preview.c', '../src/celluloid-time-label.c', '../src/celluloid-common.c'],
  dependencies: [libgtk, dependency('libadwaita-1'), dependency('mpv')],
  include_directories: include_directories('../src'),
  c_args: ['-DG_SETTINGS_ENABLE_BACKEND', '-ffunction-sections', '-fdata-sections'],
  link_args: ['-Wl,--gc-sections'])
preview_runner = find_program('test-seek-preview.sh')
test('test-seek-preview', preview_runner, args: [preview_test], timeout: 45)
