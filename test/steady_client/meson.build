project('steady-client', 'c', default_options: ['c_std=c11'])

vulkan_dep = dependency('vulkan')
xcb_dep = dependency('xcb')
xcb_icccm_dep = dependency('xcb-icccm')

executable('steady_client', 'steady_client.c',
  dependencies: [vulkan_dep, xcb_dep, xcb_icccm_dep, meson.get_compiler('c').find_library('m')],
  install: false,
)
