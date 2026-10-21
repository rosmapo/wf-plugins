xkill = shared_module(
    'xkill',
    'xkill.cpp',
    dependencies: [wayfire, wlroots, wfconfig],
    install: true,
    install_dir: wayfire.get_variable(pkgconfig: 'plugindir', default_value: get_option('libdir') / 'wayfire')
)