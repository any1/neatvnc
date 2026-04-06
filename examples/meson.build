libpng = dependency('libpng', required: false)

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
