project(
	'neatvnc',
	'c',
	version: '0.10-dev',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
		'warning_level=2',
	],
)

buildtype = get_option('buildtype')
host_system = host_machine.system()

c_args = [
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

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, 'rev-parse', '--short', 'HEAD'], check: false)
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
	if git_commit.returncode() == 0 and git_branch.returncode() == 0
		version = '"v@0@-@1@ (@2@)"'.format(
			meson.project_version(),
			git_commit.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif
add_project_arguments('-DPROJECT_VERSION=@0@'.format(version), language: 'c')

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
libsixel = dependency('libsixel', required: false)

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
	'src/auth/common.c',
	'src/server.c',
	'src/vec.c',
	'src/enc/zrle.c',
	'src/enc/raw.c',
	'src/pixels.c',
	'src/fb.c',
	'src/fb_pool.c',
	'src/rcbuf.c',
	'src/stream/interface.c',
	'src/stream/common.c',
	'src/stream/tcp.c',
	'src/desktop-layout.c',
	'src/display.c',
	'src/enc/tight.c',
	'src/enc/util.c',
	'src/qnum-to-evdev.c',
	'src/resampler.c',
	'src/transform-util.c',
	'src/damage-refinery.c',
	'src/enc/interface.c',
	'src/cursor.c',
	'src/logging.c',
	'src/base64.c',
	'src/bandwidth.c',
	'src/parallel-deflate.c',
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
	sources += [
		'src/stream/gnutls.c',
		'src/auth/vencrypt.c',
	]
	dependencies += gnutls
	config.set('ENABLE_TLS', true)
endif

if nettle.found() and hogweed.found() and gmp.found()
	dependencies += [ nettle, hogweed, gmp ]
	enable_websocket = true
	config.set('HAVE_CRYPTO', true)
	sources += [
		'src/crypto/random.c',
		'src/crypto/nettle/cipher.c',
		'src/crypto/nettle/hash.c',
		'src/crypto/nettle/key.c',
		'src/crypto/nettle/rsa.c',
		'src/stream/rsa-aes.c',
		'src/auth/apple-dh.c',
		'src/auth/rsa-aes.c',
	]
endif

if host_system == 'linux' and get_option('systemtap') and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

if gbm.found()
	dependencies += gbm
	config.set('HAVE_GBM', true)
endif

have_ffmpeg = gbm.found() and libdrm.found() and libavcodec.found() and libavfilter.found() and libavutil.found()
have_v4l2 = gbm.found() and libdrm.found() and cc.check_header('linux/videodev2.h')

if have_ffmpeg
	sources += [ 'src/enc/h264/ffmpeg-impl.c' ]
	dependencies += [libdrm, libavcodec, libavfilter, libavutil]
	config.set('HAVE_FFMPEG', true)
	config.set('HAVE_LIBAVUTIL', true)
endif

if have_v4l2
	sources += [ 'src/enc/h264/v4l2m2m-impl.c' ]
	config.set('HAVE_V4L2', true)
endif

if have_ffmpeg or have_v4l2
	sources += [ 'src/enc/h264/encoder.c', 'src/enc/h264/open-h264.c' ]
	config.set('ENABLE_OPEN_H264', true)
endif

if enable_websocket
	sources += [
		'src/stream/ws/handshake.c',
		'src/stream/ws/framing.c',
		'src/stream/ws/http.c',
		'src/stream/ws/ws.c',
	]
	config.set('ENABLE_WEBSOCKET', true)
endif

if get_option('experimental')
	if buildtype == 'release'
		warning('Experimental features enabled in release build')
	endif
	config.set('ENABLE_EXPERIMENTAL', true)
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
