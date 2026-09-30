vkroots_dep = dependency('vkroots')
dep_xcb = dependency('xcb')
dep_x11_xcb = dependency('x11-xcb')
wayland_client = dependency('wayland-client')

gamescope_wsi_layer = shared_library('VkLayer_' + wsi_layer_name + '_' + host_machine.cpu_family(), 'VkLayer_FROG_gamescope_wsi.cpp', protocols_client_src,
  cpp_args         : '-DGAMESCOPE_WSI_LAYER_NAME="VK_LAYER_@0@"'.format(wsi_layer_name),
  dependencies     : [ vkroots_dep, dep_xcb, dep_x11, dep_x11_xcb, glm_dep, wayland_client ],
  install          : true )

out_lib_dir = join_paths(prefix, lib_dir)

configure_file(
    input         : 'VkLayer_FROG_gamescope_wsi.json.in',
    output        : 'VkLayer_' + wsi_layer_name + '.' + host_machine.cpu_family() + '.json',
    configuration : {'family' : host_machine.cpu_family(), 'lib_dir' : out_lib_dir, 'layer' : wsi_layer_name, 'env' : wsi_layer_env },
    install       : true,
    install_dir   : join_paths(data_dir, 'vulkan', 'implicit_layer.d'),
    install_tag   : 'runtime'
)