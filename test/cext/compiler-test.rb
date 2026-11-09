require 'fileutils'
require 'tmpdir'
require 'open3'
require 'rbconfig'
require 'shellwords'
require_relative '../../tools/cext-record'
ROOT = File.expand_path('../..', __dir__)
def command(*args, env: {}, chdir: ROOT)
  out, err, status = Open3.capture3(env, *args, chdir: chdir)
  raise "#{args.join(' ')}\n#{out}#{err}" unless status.success?
  out
end
Dir.mktmpdir('spinel-cext-compiler-') do |tmp|
  CextRecord.record(name: 'sample', init: 'Init_sample',
    sources: [File.join(ROOT, 'test/cext/compiler/sample.c')],
    output: File.join(tmp, 'sample.json'), declarations: File.join(tmp, 'sample.rb'),
    object_archive: File.join(tmp, 'sample.a'))
  FileUtils.cp(File.join(ROOT, 'test/cext/compiler/program.rb'), File.join(tmp, 'program.rb'))
  exe = File.join(tmp, 'program')
  command(File.join(ROOT, 'bin/spinel'), File.join(tmp, 'program.rb'), '--link', File.join(tmp, 'sample.a'), '-o', exe)
  c_file = File.join(tmp, 'program.c')
  command(File.join(ROOT, 'bin/spinel'), '-c', '--no-line-map', File.join(tmp, 'program.rb'), '-o', c_file)
  c_text = File.read(c_file)
  raise 'an unrelated Array lost its typed representation' unless c_text.match?(/sp_IntArray\s*\*\s*(?:const\s+)?lv_untouched/)
  raise 'a fresh map widened its unrelated source' unless c_text.match?(/sp_IntArray\s*\*\s*(?:const\s+)?lv_source/ )
  build_recipe = command(File.join(ROOT, 'bin/spinel'), File.join(tmp, 'program.rb'), '--print-build', '--link', File.join(tmp, 'sample.a'), '-o', exe)
  raise 'print-build omitted the extension headers' unless build_recipe.lines.any? { |line| line.start_with?('include ') && File.expand_path(line.delete_prefix('include ').strip) == File.join(ROOT, 'include') }
  refusal_source = File.join(tmp, 'runtime-refusal.rb')
  File.write(refusal_source, "class CextRubyOwner; class Failure < StandardError; end; end; require_relative 'sample'; CextBox.new; Thread.new { 1 }.join\n")
  scratch = File.join(tmp, 'driver-tmp'); FileUtils.mkdir_p(scratch)
  sdk = File.join(tmp, 'wasi-sdk'); FileUtils.mkdir_p(File.join(sdk, 'bin'))
  File.write(File.join(sdk, 'bin', 'clang'), '')
  out, err, status = Open3.capture3({'TMPDIR' => scratch, 'WASI_SDK' => sdk}, File.join(ROOT, 'bin/spinel'), refusal_source, '--target=wasm32-wasi', '-o', exe + '-refused')
  raise "missing runtime refusal: #{out}#{err}" if status.success? || !err.include?('native exception runtime')
  raise 'runtime refusal leaked generated C' unless Dir[File.join(scratch, 'spinel_out_*')].empty?
  missing_reference = File.join(tmp, 'missing-reference.rb')
  File.write(missing_reference, "require_relative 'sample'; CextBox.new\n")
  out, err, status = Open3.capture3(File.join(ROOT, 'bin/spinel'), '-c', missing_reference, '-o', File.join(tmp, 'missing-reference.c'))
  raise "missing external class reference accepted: #{out}#{err}" if status.success? || !err.include?('C extension class was not registered')
  actual = command(exe)
  ['SPINEL_GC_STRESS', 'SPINEL_GC_MINOR', 'SPINEL_GC_SLAB'].each do |mode|
    value = mode == 'SPINEL_GC_SLAB' ? '0' : '1'
    stressed = command(exe, env: {mode => value, 'SPINEL_GC_VERIFY_GEN' => '1'})
    raise "#{mode} changed the output" unless stressed == actual
  end
  {
    '__cext_extension("bad", "Init_bad", 2)' => 'manifest version',
    '__cext_extension("bad", "Init_bad", 1); __cext_class("Bad", "Object"); __cext_method("Bad", "m", "instance", "public", 16, 0)' => 'arity',
    '__cext_extension("bad", "Init_bad", 1); __cext_allocator("Missing", 0)' => 'manifest class',
    '__cext_extension("bad", "Init_bad", 1); __cext_unknown(0)' => 'unknown C extension declaration',
    '__cext_extension("bad", "Init_bad", 1); __cext_class("Bad", "Object"); __cext_method("Bad", "m", "singleton", "private", 0, 0)' => 'private C extension singleton',
    '__cext_extension("bad", "Init_bad", 1); __cext_class("Bad", "Object"); __cext_constant("Bad", "Foo::BAR", "nil", nil, nil, nil)' => 'constant key must be a single name'
  }.each_with_index do |(source, reason), i|
    file = File.join(tmp, "refusal#{i}.rb")
    File.write(file, source)
    out, err, status = Open3.capture3(File.join(ROOT, 'bin/spinel'), '-c', file, '-o', File.join(tmp, "refusal#{i}.c"))
    raise "missing refusal #{reason}: #{out}#{err}" if status.success? || !err.include?(reason)
  end
  cc = Shellwords.split(ENV.fetch('CC', RbConfig::CONFIG['CC']))
  checked_bridge = File.join(tmp, 'checked-bridge.o')
  command(*cc, '-O2', '-DSP_CEXT', '-Dcalloc=counted_calloc', "-I#{ROOT}/lib", "-I#{ROOT}/include", '-c', File.join(ROOT, 'lib/sp_cext_bridge.c'), '-o', checked_bridge)
  bridge_source = File.join(tmp, 'bridge-check.c')
  FileUtils.cp(File.join(ROOT, 'test/cext/compiler/bridge-check.c'), bridge_source)
  bridge_exe = File.join(tmp, 'bridge-check')
  command(*cc, '-O2', "-I#{ROOT}/lib", "-I#{ROOT}/include", bridge_source, checked_bridge, File.join(tmp, 'sample.a'), File.join(ROOT, 'lib/libspinel_cext_rt.a'), '-lm', '-o', bridge_exe)
  command(bridge_exe)
  command(bridge_exe, env: {'SPINEL_GC_STRESS' => '1'})
  native = File.join(tmp, 'cruby')
  FileUtils.mkdir_p(native)
  FileUtils.cp(File.join(ROOT, 'test/cext/compiler/sample.c'), File.join(native, 'sample.c'))
  FileUtils.cp(File.join(ROOT, 'test/cext/compiler/program.rb'), File.join(native, 'program.rb'))
  command(RbConfig.ruby, '-rmkmf', '-e', 'create_makefile("sample")', chdir: native)
  command('make', chdir: native)
  expected = command(RbConfig.ruby, File.join(native, 'program.rb'))
  raise "CRuby differs\nexpected:\n#{expected}\nactual:\n#{actual}" unless actual == expected
end
# An extension and its program, compiled plain, with shared Strings and under GC
# stress, each printing what CRuby prints with the same C built by mkmf.
def cruby_oracle(name)
  Dir.mktmpdir("spinel-cext-#{name}-") do |tmp|
    source = File.join(ROOT, "test/cext/compiler/#{name}.c")
    program = File.join(ROOT, "test/cext/compiler/#{name}.rb")
    archive = File.join(tmp, "#{name}.a")
    CextRecord.record(name: name, init: "Init_#{name}", sources: [source],
      output: File.join(tmp, "#{name}.json"), declarations: File.join(tmp, "#{name}.rb"),
      object_archive: archive)
    FileUtils.cp(program, File.join(tmp, 'program.rb'))
    exe = File.join(tmp, 'program')
    command(File.join(ROOT, 'bin/spinel'), File.join(tmp, 'program.rb'), '--link', archive, '-o', exe)
    actual = command(exe)
    stressed = command(exe, env: {'SPINEL_GC_STRESS' => '1', 'SPINEL_GC_VERIFY_GEN' => '1'})
    raise "GC stress changed the #{name} output" unless stressed == actual
    native = File.join(tmp, 'cruby')
    FileUtils.mkdir_p(native)
    FileUtils.cp(source, File.join(native, "#{name}.c"))
    FileUtils.cp(program, File.join(native, 'program.rb'))
    command(RbConfig.ruby, '-rmkmf', '-e', "create_makefile(#{name.dump})", chdir: native)
    command('make', chdir: native)
    expected = command(RbConfig.ruby, File.join(native, 'program.rb'))
    recorded = File.join(ROOT, "test/cext/compiler/#{name}.rb.expected")
    raise "stale #{name} CRuby expectation" if File.file?(recorded) && File.read(recorded) != expected
    raise "CRuby differs on #{name}\nexpected:\n#{expected}\nactual:\n#{actual}" unless actual == expected
  end
end
cruby_oracle('display')
cruby_oracle('dispatch')
cruby_oracle('object_class')
cruby_oracle('numbers')
cruby_oracle('types')
cruby_oracle('arrays')
cruby_oracle('catch_throw')
cruby_oracle('sealed')
cruby_oracle('made')
cruby_oracle('const_defined')
cruby_oracle('symbol_ids')
cruby_oracle('feed')
cruby_oracle('formats')
cruby_oracle('hidden_ivars')
cruby_oracle('raise_program_class')
cruby_oracle('conversions')
cruby_oracle('spread_sends')
cruby_oracle('callback_versions')
puts 'C extension compiler bridge: CRuby oracle and GC modes passed'
