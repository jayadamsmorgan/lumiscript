if (CONFIG_LUMISCRIPT_VM)

    zephyr_library()

    zephyr_library_include_directories(include)

    zephyr_library_sources_ifdef(CONFIG_LUMISCRIPT_VM src/vm.c)

    zephyr_library_sources_ifdef(
        CONFIG_LUMISCRIPT_COMPILER
        src/bytecode.c
        src/compiler.c
        src/lexer.c
        src/parser.c
    )

endif()
