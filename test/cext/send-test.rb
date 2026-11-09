require 'fileutils'
require 'open3'
require 'rbconfig'
require 'shellwords'
require_relative '../../tools/cext-record'
ENV["SPINEL_CEXT_STAGE6_ENV"] = "from literal eval"
ROOT = File.expand_path('../..', __dir__)
def command(*args, env: {}, chdir: ROOT)
  out, err, status = Open3.capture3(env, *args, chdir: chdir)
  raise "#{args.join(' ')} (#{status})\n#{out}#{err}" unless status.success?
  out
end
Dir.mktmpdir('spinel-cext-send-') do |tmp|
  CextRecord.record(name: 'send_sample', init: 'Init_send_sample',
    sources: [File.join(ROOT, 'test/cext/send/sample.c')],
    output: File.join(tmp, 'send_sample.json'), declarations: File.join(tmp, 'send_sample.rb'),
    object_archive: File.join(tmp, 'send_sample.a'))
  FileUtils.cp(File.join(ROOT, 'test/cext/send/program.rb'), File.join(tmp, 'program.rb'))
  native = File.join(tmp, 'native'); FileUtils.mkdir_p(native)
  FileUtils.cp(File.join(ROOT, 'test/cext/send/sample.c'), File.join(native, 'send_sample.c'))
  FileUtils.cp(File.join(ROOT, 'test/cext/send/program.rb'), File.join(native, 'program.rb'))
  command(RbConfig.ruby, '-rmkmf', '-e', 'create_makefile("send_sample")', chdir: native)
  command('make', chdir: native)
  expected = command(RbConfig.ruby, File.join(native, 'program.rb'))
  exe = File.join(tmp, 'program')
  command(File.join(ROOT, 'bin/spinel'), File.join(tmp, 'program.rb'),
    '--link', File.join(tmp, 'send_sample.a'), '-o', exe)
  [{}, {'SPINEL_GC_STRESS' => '1'}, {'SPINEL_GC_MINOR' => '1'}, {'SPINEL_GC_SLAB' => '0'}].each do |env|
    actual = command(exe, env: env.merge('SPINEL_GC_VERIFY_GEN' => '1'))
    raise "CRuby differs (#{env})\nexpected:\n#{expected}\nactual:\n#{actual}" unless actual == expected
  end
  c_file = File.join(tmp, 'program.c')
  command(File.join(ROOT, 'bin/spinel'), '-c', '--no-line-map', File.join(tmp, 'program.rb'), '-o', c_file)
  FileUtils.cp(File.join(ROOT, 'test/cext/send/bridge-check.c'), File.join(tmp, 'bridge-check.c'))
  cc = Shellwords.split(ENV.fetch('CC', RbConfig::CONFIG['CC']))
  bridge_exe = File.join(tmp, 'bridge-check')
  command(*cc, '-O2', '-Wno-all', "-I#{ROOT}/lib", "-I#{ROOT}/include", File.join(tmp, 'bridge-check.c'),
    File.join(tmp, 'send_sample.a'), File.join(ROOT, 'lib/libspinel_cext_rt.a'), '-lm', '-o', bridge_exe)
  command(bridge_exe)
  command(bridge_exe, env: {'SPINEL_GC_STRESS' => '1'})
  _out, err, status = Open3.capture3(bridge_exe, 'known', rlimit_core: 0)
  raise "uncompiled known method pair did not abort: #{err}" unless status.signaled? && err.include?('[BUG] C extension called known method answer with uncompiled arity 19')

end
puts 'C extension send table: CRuby oracle and GC modes passed'
