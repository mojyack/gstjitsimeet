project('gstjitsimeet', 'cpp', version : '0.0', default_options : ['warning_level=3', 'cpp_std=c++20'])
add_global_arguments('-Wfatal-errors', language: 'cpp')

gstreamer_dep = dependency('gstreamer-1.0', version : ['>=1.20'])

deps = [
  gstreamer_dep,
  dependency('gstreamer-rtp-1.0'),
  dependency('threads'),
  dependency('openssl'),
]

src = files(
  'src/lib.cpp',
  'src/jitsibin.cpp',
  'src/props.cpp',
)

subdir('src/jitsi')

library('gstjitsimeet', src + libjitsimeet_src,
        dependencies: deps + libjitsimeet_deps,
        link_args: ['-Wl,-z,stack-size=0x400000'], # TODO: really needed even in musl system?
        install : true,
)

executable('example', 
        files('src/example.cpp'),
        dependencies: [gstreamer_dep]
) 

# TODO: remove this example
executable('dynamic-switch-example', 
        files('src/dynamic-switch-example.cpp'),
        dependencies: [gstreamer_dep]
) 
