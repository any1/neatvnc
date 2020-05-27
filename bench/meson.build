libpng = dependency('libpng', required: false)

if libpng.found()
	executable(
		'zrle-bench',
		[
			'zrle-bench.c',
			'../src/zrle.c',
			'../src/pngfb.c',
			'../src/pixels.c',
			'../src/vec.c',
		],
		dependencies: [
			neatvnc_dep,
			pixman,
			aml,
			libpng,
		]
	)
endif
