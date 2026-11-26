require 'tmpdir'
require 'fileutils'
require 'open3'
require 'rbconfig'
require_relative '../../tools/cext-record'
ROOT = File.expand_path('../..', __dir__)
def run(*args, env: {})
  out, err, status = Open3.capture3(env, *args)
  raise "#{args.join(' ')}\n#{out}#{err}" unless status.success?
  out
end
PARKED = "[500, 500, 500, 500]\n10\n20\n"
Dir.mktmpdir('spinel-cxl-') do |tmp|
  CextRecord.record(name: 'cxl_sample', init: 'Init_cxl_sample',
    libs: [],
    sources: [File.join(ROOT, 'test/cext/threads/sample.c')],
    output: File.join(tmp, 'cxl_sample.json'), declarations: File.join(tmp, 'cxl_sample.rb'),
    object_archive: File.join(tmp, 'cxl_sample.a'))
  source = File.join(tmp, 'program.rb')
  FileUtils.cp(File.join(ROOT, 'test/cext/threads/program.rb'), source)
  exe = File.join(tmp, 'program')
  run(File.join(ROOT, 'bin/spinel'), source, '--link', File.join(tmp, 'cxl_sample.a'), '-o', exe)
  [1, 2, 4].each do |workers|
    actual = run(exe, env: {'SPINEL_WORKERS' => workers.to_s})
    raise "unexpected output: #{actual}" unless actual == "80\n"
    stressed = run(exe, env: {'SPINEL_WORKERS' => workers.to_s, 'SPINEL_GC_STRESS' => '1', 'SPINEL_GC_VERIFY_GEN' => '1'})
    raise "GC stress changed output: #{stressed}" unless stressed == actual
  end
  source = File.join(tmp, 'interrupt.rb')
  FileUtils.cp(File.join(ROOT, 'test/cext/threads/interrupt.rb'), source)
  interrupt_exe = File.join(tmp, 'interrupt')
  run(File.join(ROOT, 'bin/spinel'), source, '--link', File.join(tmp, 'cxl_sample.a'), '-o', interrupt_exe)
  [2, 4].each do |workers|
    actual = run(interrupt_exe, env: {'SPINEL_WORKERS' => workers.to_s})
    raise "unexpected interrupt output: #{actual}" unless actual == "kill ensured\ninterrupted\nraise ensured\n1\n"
  end
  source = File.join(tmp, 'parked.rb')
  FileUtils.cp(File.join(ROOT, 'test/cext/threads/parked.rb'), source)
  parked_exe = File.join(tmp, 'parked')
  run(File.join(ROOT, 'bin/spinel'), source, '--link', File.join(tmp, 'cxl_sample.a'), '-o', parked_exe)
  [1, 2, 4].each do |workers|
    actual = run(parked_exe, env: {'SPINEL_WORKERS' => workers.to_s})
    raise "unexpected parked output: #{actual}" unless actual == PARKED
  end
  native = File.join(tmp, 'native'); FileUtils.mkdir_p(native)
  FileUtils.cp(File.join(ROOT, 'test/cext/threads/sample.c'), File.join(native, 'cxl_sample.c'))
  %w[program interrupt parked].each do |name|
    FileUtils.cp(File.join(ROOT, "test/cext/threads/#{name}.rb"), File.join(native, "#{name}.rb"))
  end
  Dir.chdir(native) do
    run(RbConfig.ruby, '-rmkmf', '-e', 'create_makefile("cxl_sample")')
    run('make')
    raise 'CRuby serialization differs' unless run(RbConfig.ruby, 'program.rb') == "80\n"
    raise 'CRuby interruption differs' unless run(RbConfig.ruby, 'interrupt.rb') == "kill ensured\ninterrupted\nraise ensured\n1\n"
    raise 'CRuby parked differs' unless run(RbConfig.ruby, 'parked.rb') == PARKED
  end
end
puts 'CXL threaded bridge: CRuby oracle and GC stress passed'
