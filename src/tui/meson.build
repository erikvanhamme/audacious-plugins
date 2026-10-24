notcurses_dep = dependency('notcurses', version: '>= 3.0', required: false)
have_tui = notcurses_dep.found()


if have_tui
  shared_module('tui',
    'tui.cc',
    dependencies: [audacious_dep, glib_dep, notcurses_dep],
    name_prefix: '',
    install: true,
    install_dir: general_plugin_dir
  )
endif
