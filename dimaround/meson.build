project('wayfire-dimaround', 'cpp',
    version: '0.1.0',
    default_options: ['cpp_std=c++17', 'warning_level=2'])

add_project_arguments(['-DWLR_USE_UNSTABLE', '-DWAYFIRE_PLUGIN'], language: 'cpp')

wayfire  = dependency('wayfire')
wfconfig = dependency('wf-config')
wlroots  = dependency('wlroots-0.18')

shared_module('dimaround', 'dimaround.cpp',
    name_prefix: 'lib',
    dependencies: [wayfire, wfconfig, wlroots],
    install: true,
    install_dir: wayfire.get_variable(pkgconfig: 'plugindir'))

install_data('dimaround.xml',
    install_dir: wayfire.get_variable(pkgconfig: 'metadatadir'))
