libpng = dependency('libpng', required: false)

configure_file(
	output: 'config.h',
	configuration: config,
)

if libpng.found()

	executable(
		'zrle-bench',
		[
			'zrle-bench.c',
			'../src/enc/util.c',
			'../src/enc/zrle.c',
			'../src/enc/tight.c',
			'../src/enc/raw.c',
			'../src/enc/h264/open-h264.c',
			'../src/enc/h264/encoder.c',
			'../src/enc/h264/ffmpeg-impl.c',
			'../src/enc/h264/v4l2m2m-impl.c',
			'../src/enc/interface.c',
			'../src/pngfb.c',
			'../src/pixels.c',
			'../src/vec.c',
			'../src/rcbuf.c',
			'../src/fb.c',
		],
		dependencies: [
			neatvnc_dep,
			pixman,
			aml,
			libpng,
			libm,
			zlib,
			libturbojpeg,
			gbm,
			libdrm,
			libavutil,
			libavcodec,
			libavfilter,
		],
	)
endif

executable(
	'parallel-deflate',
	[
		'parallel-deflate-bench.c',
		'../src/parallel-deflate.c',
		'../src/vec.c',
	],
	dependencies: [
		neatvnc_dep,
		aml,
		zlib,
		libm,
	],
)
