libpng = dependency('libpng', required: false)
libavformat = dependency('libavformat', required: false)

executable(
	'draw',
	[
		'draw.c',
	],
	dependencies: [
		neatvnc_dep,
		pixman,
		aml,
		libm,
		libdrm_inc,
	]
)

if gbm.found() and libavcodec.found() and libavutil.found() and \
   libavformat.found() and libavfilter.found()
	executable(
		'video-player',
		[
			'video-player.c',
		],
		dependencies: [
			neatvnc_dep,
			aml,
			gbm,
			libdrm_inc,
			libm,
			libavformat,
			libavcodec,
			libavutil,
			libavfilter,
		]
	)
endif

if libpng.found()
	executable(
		'png-server',
		[
			'png-server.c',
			'../src/pngfb.c',
		],
		dependencies: [
			neatvnc_dep,
			pixman,
			aml,
			libpng,
                        libdrm_inc,
		]
	)

	executable(
		'multi-display',
		[
			'multi-display.c',
			'../src/pngfb.c',
		],
		dependencies: [
			neatvnc_dep,
			pixman,
			aml,
			libpng,
                        libdrm_inc,
		]
	)
endif
