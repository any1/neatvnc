configure_file(
	output: 'config.h',
	configuration: config,
)

if libpng.found()
	executable('zrle-bench', 'zrle-bench.c',
		include_directories: inc,
		dependencies: [ dependencies, pngfb ],
	)

endif

if libpng.found() and libsixel.found()
	executable('compositor-bench', 'compositor-bench.c',
		include_directories: inc,
		dependencies: [ dependencies, pngfb, libsixel ],
	)
endif

executable('parallel-deflate', 'parallel-deflate-bench.c',
        include_directories: inc,
	dependencies: dependencies,
)
