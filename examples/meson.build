libpng = dependency('libpng', required: false)

executable(
	'draw',
	[
		'draw.c',
	],
	dependencies: [
		neatvnc_dep,
		pixman,
		libuv,
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
			libuv,
			libpng,
		]
	)
endif
