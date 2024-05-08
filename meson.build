project('gstjitsimeet', 'cpp', version : '0.0', default_options : ['warning_level=3', 'cpp_std=c++20'])
add_global_arguments('-Wfatal-errors', language: 'cpp')

deps = [
  dependency('gstreamer-1.0', version : ['>=1.16']),
  dependency('gstreamer-rtp-1.0'),
  dependency('threads'),
  dependency('openssl'),
  dependency('nice'),
]

src = files(
  'src/lib.cpp',
  'src/jitsibin.cpp',
  'src/jingle.cpp',
  'src/cert.cpp',
  'src/pem.cpp',
  'src/hostaddr.cpp',
  'src/ice.cpp',
)

subdir('src/jitsi')

library('gstjitsimeet', src + libjitsimeet_src,
        dependencies: deps + libjitsimeet_deps,
        link_args: ['-Wl,-z,stack-size=0x400000'], # TODO: really needed even in musl system?
        install : true,
)

executable('example', 
        files('src/example.cpp'),
        dependencies: dependency('gstreamer-1.0'), # TODO: refactor
) 
