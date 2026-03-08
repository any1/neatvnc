pixels = executable('pixels',
	[
		'test-pixels.c',
		'../src/pixels.c',
	],
	include_directories: inc,
	dependencies: [
		pixman,
		libdrm_inc,
		libm,
	],
)
test('pixels', pixels)

base64 = executable('base64',
	[
		'test-base64.c',
		'../src/base64.c',
	],
	include_directories: inc,
)
test('base64', base64)

if nettle.found()
	rfb_test_server = executable('rfb-test-server',
		['rfb-test-server.c', '../src/crypto/nettle/des-rfb.c'],
		include_directories: inc,
		dependencies: [
			neatvnc_dep,
			pixman,
			aml,
			nettle,
			gnutls,
			libdrm_inc,
		],
	)

	python3 = find_program('python3', required: false)
	if python3.found()
		test('rfb-handshake', python3,
			args: [files('test_rfb_handshake.py'), rfb_test_server.full_path()],
			depends: [rfb_test_server],
			timeout: 30,
		)
	endif
endif
