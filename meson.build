project(
	'neatvnc',
	'c',
	version: '0.6.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
		'warning_level=2',
	],
)

buildtype = get_option('buildtype')
host_system = host_machine.system()

c_args = [
	'-DPROJECT_VERSION="@0@"'.format(meson.project_version()),
	'-D_GNU_SOURCE',
	'-fvisibility=hidden',
	'-DAML_UNSTABLE_API=1',

	'-Wmissing-prototypes',
	'-Wno-unused-parameter',
	'-Wno-format-truncation',
]

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

git = find_program('git', native: true, required: false)
if git.found()
	git_describe = run_command([git, 'describe', '--tags', '--long'], check: false)
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
	if git_describe.returncode() == 0 and git_branch.returncode() == 0
		c_args += '-DGIT_VERSION="@0@ (@1@)"'.format(
			git_describe.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

libdrm_inc = dependency('libdrm').partial_dependency(compile_args: true)

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)

pixman = dependency('pixman-1')
libturbojpeg = dependency('libturbojpeg', required: get_option('jpeg'))
gnutls = dependency('gnutls', required: get_option('tls'))
nettle = dependency('nettle', required: get_option('nettle'))
hogweed = dependency('hogweed', required: get_option('nettle'))
gmp = dependency('gmp', required: get_option('nettle'))
zlib = dependency('zlib')
gbm = dependency('gbm', required: get_option('gbm'))
libdrm = dependency('libdrm', required: get_option('h264'))

libavcodec = dependency('libavcodec', required: get_option('h264'))
libavfilter = dependency('libavfilter', required: get_option('h264'))
libavutil = dependency('libavutil', required: get_option('h264'))

aml_version = ['>=0.3.0', '<0.4.0']
aml_project = subproject('aml', required: false, version: aml_version)
if aml_project.found()
	aml = aml_project.get_variable('aml_dep')
else
	aml = dependency('aml', version: aml_version)
endif

inc = include_directories('include')

sources = [
	'src/server.c',
	'src/vec.c',
	'src/zrle.c',
	'src/raw-encoding.c',
	'src/pixels.c',
	'src/fb.c',
	'src/fb_pool.c',
	'src/rcbuf.c',
	'src/stream.c',
	'src/stream-common.c',
	'src/stream-tcp.c',
	'src/desktop-layout.c',
	'src/display.c',
	'src/tight.c',
	'src/enc-util.c',
	'src/qnum-to-evdev.c',
	'src/resampler.c',
	'src/transform-util.c',
	'src/damage-refinery.c',
	'src/encoder.c',
	'src/cursor.c',
	'src/logging.c',
	'src/base64.c',
]

dependencies = [
	libm,
	pixman,
	aml,
	zlib,
	libdrm_inc,
]

enable_websocket = false

config = configuration_data()

if libturbojpeg.found()
	dependencies += libturbojpeg
	config.set('HAVE_JPEG', true)
endif

if gnutls.found()
	sources += 'src/stream-gnutls.c'
	dependencies += gnutls
	config.set('ENABLE_TLS', true)
endif

if nettle.found() and hogweed.found() and gmp.found()
	dependencies += [ nettle, hogweed, gmp ]
	enable_websocket = true
	config.set('HAVE_CRYPTO', true)
	sources += ['src/crypto-nettle.c', 'src/stream-rsa-aes.c']
endif

if host_system == 'linux' and get_option('systemtap') and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

if gbm.found()
	dependencies += gbm
	config.set('HAVE_GBM', true)
endif

if gbm.found() and libdrm.found() and libavcodec.found() and libavfilter.found() and libavutil.found()
	sources += [ 'src/h264-encoder.c', 'src/open-h264.c' ]
	dependencies += [libdrm, libavcodec, libavfilter, libavutil]
	config.set('ENABLE_OPEN_H264', true)
	config.set('HAVE_LIBAVUTIL', true)
endif

if enable_websocket
	sources += [
		'src/ws-handshake.c',
		'src/ws-framing.c',
		'src/http.c',
		'src/stream-ws.c',
	]
	config.set('ENABLE_WEBSOCKET', true)
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

if get_option('tests')
	subdir('test')
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
