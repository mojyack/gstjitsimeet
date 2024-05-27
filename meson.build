project('gstjitsimeet', 'cpp', version : '0.0', default_options : ['warning_level=3', 'cpp_std=c++20'])
add_project_arguments('-Wfatal-errors', language: 'cpp')
add_project_arguments('-fpermissive', language: 'cpp')

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
        install : true,
)

# examples
executable('receiver-example', 
        files('src/gstutil/pipeline-helper.cpp', 'src/examples/receiver.cpp'),
        dependencies: [gstreamer_dep]
) 
executable('room-to-room-example', 
        files('src/gstutil/pipeline-helper.cpp', 'src/examples/room-to-room.cpp'),
        dependencies: [gstreamer_dep]
) 
# TODO: move this example to another repository
#       here is not a correct place for this, since this is unrelated to jitsi
executable('dynamic-pipeline-switch-example', 
        files('src/gstutil/pipeline-helper.cpp', 'src/examples/dynamic-pipeline-switch.cpp'),
        dependencies: [gstreamer_dep]
) 
