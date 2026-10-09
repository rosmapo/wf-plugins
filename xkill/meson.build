project(
    'wayfire-xkill',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=0.51.0',
    default_options: [
        'cpp_std=c++17',
        'warning_level=2',
        'werror=false',
    ],
)

wayfire  = dependency('wayfire')
wlroots  = dependency('wlroots')
wfconfig = dependency('wf-config')

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp'])
add_project_arguments(['-DWAYFIRE_PLUGIN'], language: ['cpp'])
add_project_link_arguments(['-rdynamic'], language: 'cpp')

subdir('src')
subdir('metadata')

summary = [
    '',
    '----------------',
    'wayfire-xkill @0@'.format(meson.project_version()),
    '----------------',
    '',
]
message('\n'.join(summary))
