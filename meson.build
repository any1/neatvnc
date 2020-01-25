project(
	'neatvnc',
	'c',
	version: '0.0.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

buildtype = get_option('buildtype')

c_args = [
	'-D_GNU_SOURCE',
	'-fvisibility=hidden',
]

if buildtype == 'release' or buildtype == 'plain'
	c_args += '-DNDEBUG'
endif

cpu = host_machine.cpu_family()

if cpu == 'x86_64'
	c_args += '-mavx'
elif cpu == 'arm'
	c_args += '-mfpu=neon'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)

pixman = dependency('pixman-1')
libuv = dependency('libuv')
libturbojpeg = dependency('libturbojpeg', required: get_option('tight-encoding'))
gnutls = dependency('gnutls', required: get_option('tls'))

inc = include_directories('include', 'contrib/miniz')

sources = [
	'src/server.c',
	'src/vec.c',
	'src/zrle.c',
	'src/raw-encoding.c',
	'src/pixels.c',
	'src/damage.c',
	'src/fb.c',
	'src/rcbuf.c',
	'src/stream.c',
	'contrib/miniz/miniz.c',
]

dependencies = [
	libm,
	pixman,
	libuv,
]

config = configuration_data()

if libturbojpeg.found()
	dependencies += libturbojpeg
	sources += 'src/tight.c'
	config.set('ENABLE_TIGHT', true)
endif

if gnutls.found()
	dependencies += gnutls
	config.set('ENABLE_TLS', true)
endif

configure_file(
	output: 'config.h',
	configuration: config,
)

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
