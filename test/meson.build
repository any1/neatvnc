pixels = executable('pixels',
	[
		'test-pixels.c',
		'../src/pixels.c',
	],
	include_directories: inc,
	dependencies: [
		pixman,
		libdrm_inc,
	],
)
test('pixels', pixels)
