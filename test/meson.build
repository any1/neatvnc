configure_file(
	output: 'config.h',
	configuration: config,
)

pixels = executable('pixels', 'test-pixels.c',
	include_directories: inc,
	dependencies: dependencies
)
test('pixels', pixels)

base64 = executable('base64', 'test-base64.c',
	include_directories: inc,
	dependencies: dependencies
)
test('base64', base64)

if nettle.found() and python3.found()
	rfb_test_server = executable('rfb-test-server', 'rfb-test-server.c',
		include_directories: inc,
		dependencies: dependencies,
	)

	test('rfb-handshake', python3,
		args: [files('test_rfb_handshake.py'), rfb_test_server.full_path()],
		depends: [rfb_test_server],
		timeout: 30,
	)
endif
