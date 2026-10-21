project(
    'rounded-corners',
    'cpp',
    version: '0.2.0',
    license: 'MIT',
    meson_version: '>=0.59.0',
    default_options: ['cpp_std=c++17'],
)

add_project_arguments(['-DWLR_USE_UNSTABLE', '-DWAYFIRE_PLUGIN'], language: 'cpp')
add_project_link_arguments(['-rdynamic', '-fPIC'], language: 'cpp')

wayfire = dependency('wayfire', version: '>=0.9.0')
glm = dependency('glm')

shared_module('rounded-corners', 'rounded-corners.cpp',
    dependencies: [wayfire, glm],
    install: true,
    install_dir: join_paths(get_option('libdir'), 'wayfire'))

install_data('rounded-corners.xml',
    install_dir: wayfire.get_variable(pkgconfig: 'metadatadir'))
