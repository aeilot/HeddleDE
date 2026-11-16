project('heddle', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

add_project_arguments('-DWLR_USE_UNSTABLE', '-Wno-unused-parameter', language: 'c')

wlroots = dependency('wlroots-0.18')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
libpng = dependency('libpng')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)

wl_protocol_dir = dependency('wayland-protocols').get_variable('pkgdatadir')
wayland_scanner = find_program(dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))
xdg_shell_h = custom_target('xdg-shell-protocol.h',
  input: wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

executable('heddle', 'heddle.c', xdg_shell_h,
  dependencies: [wlroots, wayland_server, xkbcommon, pixman, libpng, libdrm],
  install: true)
