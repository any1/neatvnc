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
		]
	)
endif
