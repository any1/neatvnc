project(
	'neatvnc',
	'c',
	version: '0.2.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

buildtype = get_option('buildtype')
host_system = host_machine.system()

c_args = [
	'-DPROJECT_VERSION="@0@"'.format(meson.project_version()),
	'-D_GNU_SOURCE',
	'-fvisibility=hidden',
	'-Wmissing-prototypes',
]

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

git = find_program('git', native: true, required: false)
if git.found()
	git_describe = run_command([git, 'describe', '--tags', '--long'])
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'])
	if git_describe.returncode() == 0 and git_branch.returncode() == 0
		c_args += '-DGIT_VERSION="@0@ (@1@)"'.format(
			git_describe.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)

pixman = dependency('pixman-1')
libturbojpeg = dependency('libturbojpeg', required: get_option('jpeg'))
gnutls = dependency('gnutls', required: get_option('tls'))
zlib = dependency('zlib')

aml_project = subproject('aml', required: false)
if aml_project.found()
	aml = aml_project.get_variable('aml_dep')
else
	aml = dependency('aml')
endif

inc = include_directories('include')

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
	'src/display.c',
	'src/tight.c',
	'src/enc-util.c',
]

dependencies = [
	libm,
	pixman,
	aml,
	zlib,
]

config = configuration_data()

if libturbojpeg.found()
	dependencies += libturbojpeg
	config.set('HAVE_JPEG', true)
endif

if gnutls.found()
	dependencies += gnutls
	config.set('ENABLE_TLS', true)
endif

if host_system == 'linux' and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

configure_file(
	output: 'config.h',
	configuration: config,
)

neatvnc = library(
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

if get_option('examples')
	subdir('examples')
endif

if get_option('benchmarks')
	subdir('bench')
endif

install_headers('include/neatvnc.h')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
	libraries: neatvnc,
	version: meson.project_version(),
	filebase: meson.project_name(),
	name: meson.project_name(),
	description: 'A Neat VNC server library'
)
