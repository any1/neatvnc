project(
	'neatvnc',
	'c',
	version: '0.0.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

add_project_arguments([
	'-D_GNU_SOURCE',
	'-fvisibility=hidden',
], language: 'c')

cpu = host_machine.cpu_family()

if cpu == 'x86_64'
	arch_args = [
		'-mavx',
	]
elif cpu == 'arm' or cpu == 'aarch64'
	arch_args = [
		'-mfpu=neon',
	]
else
	arch_args = []
endif

add_project_arguments(arch_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)

pixman = dependency('pixman-1')
libuv = dependency('libuv')

inc = include_directories('include', 'contrib/miniz')

sources = [
	'src/server.c',
	'src/util.c',
	'src/vec.c',
	'src/zrle.c',
	'src/raw-encoding.c',
	'src/pixels.c',
	'src/damage.c',
	'src/fb.c',
	'contrib/miniz/miniz.c',
]

dependencies = [
	libm,
	pixman,
	libuv,
]

neatvnc = shared_library(
	'neatvnc',
	sources,
	version: '0.0.0',
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)

neatvnc_dep = declare_dependency(
	include_directories: inc,
	link_with: neatvnc,
)

install_headers('include/neatvnc.h')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
	libraries: neatvnc,
	version: meson.project_version(),
	filebase: meson.project_name(),
	name: meson.project_name(),
	description: 'A Neat VNC server library'
)
