project('lumiscript', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=3'])

cc = meson.get_compiler('c')
libm = cc.find_library('m', required: true)

inc = include_directories('include')

liblumiscript = library(
  'lumiscript',
  [
    'src/bytecode.c',
    'src/compiler.c',
    'src/lexer.c',
    'src/parser.c',
    'src/vm.c',
  ],
  include_directories: inc,
  dependencies: libm,
  install: true,
)

lumiscript_dep = declare_dependency(
  include_directories: inc,
  link_with: liblumiscript,
)

executable(
  'lumic',
  'src/main.c',
  include_directories: inc,
  dependencies: lumiscript_dep,
  install: true,
)

executable(
  'lumivm',
  'src/vm_main.c',
  include_directories: inc,
  dependencies: lumiscript_dep,
  install: true,
)

executable(
  'bytecode_assert_sections',
  'tests/bytecode_assert_sections.c',
  install: false,
)

test(
  'compile-static-example',
  find_program('tests/compile_example.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-static.lumi', 'static.lbc'],
)

test(
  'compile-animation-example',
  find_program('tests/compile_example.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-animation.lumi', 'animation.lbc'],
)

test(
  'compile-stateful-example',
  find_program('tests/compile_example.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-stateful.lumi', 'stateful.lbc'],
)

test(
  'compile-conditional-example',
  find_program('tests/compile_example.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-conditional.lumi', 'conditional.lbc'],
)

test(
  'compile-nested-example',
  find_program('tests/compile_example.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-nested.lumi', 'nested.lbc'],
)

test(
  'compile-math-example',
  find_program('tests/compile_example.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-math.lumi', 'math.lbc'],
)

test(
  'reject-assign-let',
  find_program('tests/compile_fail.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'tests/error-assign-let.lumi', 'can be reassigned'],
)

test(
  'reject-unknown-symbol',
  find_program('tests/compile_fail.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'tests/error-unknown-symbol.lumi', 'unknown symbol'],
)

test(
  'reject-missing-color',
  find_program('tests/compile_fail.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'tests/error-missing-color.lumi', 'render section must assign color on all control-flow paths'],
)

test(
  'reject-render-missing-else-color',
  find_program('tests/compile_fail.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'tests/error-render-missing-else-color.lumi', 'render section must assign color on all control-flow paths'],
)

test(
  'reject-if-expr-without-else',
  find_program('tests/compile_fail.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'tests/error-if-expr-no-else.lumi', 'if expression requires else branch'],
)

test(
  'run-static-vm',
  find_program('tests/compile_and_run_vm.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-static.lumi', 'vm-static.lbc', 'tests/vm-static-input.txt', 'tests/vm-static-expected.txt'],
)

test(
  'assert-static-bytecode-sections',
  find_program('tests/compile_and_assert_sections.sh'),
  args: [
    meson.project_source_root(),
    meson.project_build_root(),
    'example-static.lumi',
    'static-sections.lbc',
    '1',
    '3',
    '0',
    '0',
    '3',
    '150,200,255',
    '-',
    '-',
    '26',
    '26',
    '1,0,0,1,1,0,1,2,0,22,4,3,25,26',
  ],
)

test(
  'run-branching-vm',
  find_program('tests/compile_and_run_vm.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-branching.lumi', 'vm-branching.lbc', 'tests/vm-branching-input.txt', 'tests/vm-branching-expected.txt'],
)

test(
  'run-counter-vm',
  find_program('tests/compile_and_run_vm.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-counter.lumi', 'vm-counter.lbc', 'tests/vm-counter-input.txt', 'tests/vm-counter-expected.txt'],
)

test(
  'run-math-vm',
  find_program('tests/compile_and_run_vm.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-math.lumi', 'vm-math.lbc', 'tests/vm-math-input.txt', 'tests/vm-math-expected.txt'],
)

test(
  'reject-vm-small-stack',
  find_program('tests/run_vm_fail.sh'),
  args: [meson.project_source_root(), meson.project_build_root(), 'example-branching.lumi', 'vm-small-stack.lbc', '--stack-cap', '1', 'capacity check failed: runtime stack too small'],
)
