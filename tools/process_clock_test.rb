# Exercise the real clock lowering with an OS clock seam in child processes.
require "tmpdir"
require "shellwords"
root = File.expand_path("..", __dir__)
compiler = File.expand_path(ARGV[0] || "bin/spinel")
reference = ENV["REF_RUBY"] || RbConfig.ruby
source = root + "/tools/fixtures/clock_uuid_v7.rb"
darwin = RUBY_PLATFORM.include?("darwin")
unless darwin || RUBY_PLATFORM.include?("linux")
  puts "process-clock-test: skipped (requires macOS or Linux clock interposition)"
  exit
end
Dir.mktmpdir("spinel-clock-") do |dir|
  shim = dir + (darwin ? "/clock.dylib" : "/clock.so")
  cc = Shellwords.split(ENV["CC"] || "cc")
  args = darwin ? ["-dynamiclib"] : ["-shared", "-fPIC"]
  args += ["-O2", root + "/tools/fixtures/clock_interpose.c", "-o", shim]
  args << "-ldl" unless darwin
  abort "process-clock-test: FAIL (shim build)" unless system(*cc, *args)
  binaries = []
  [["default", []], ["promote", ["--int-overflow=promote"]], ["debug", ["--debug"]]].each do |mode, flags|
    binary = dir + "/" + mode
    log = dir + "/" + mode + ".log"
    unless system(compiler, *flags, source, "-o", binary, out: log, err: log)
      abort "process-clock-test: FAIL (#{mode} compilation; #{File.read(log).lines.first})"
    end
    binaries << [mode, binary]
  end
  count = 0
  [-1, -244, -999_999, -1_000_000, -1_000_001, -1_000_000_001, 0, 1_234_567_890_123_654_321].each do |ns|
    env = {darwin ? "DYLD_INSERT_LIBRARIES" : "LD_PRELOAD" => shim,
           "SPINEL_TEST_CLOCK_NS" => ns.to_s}
    expected = IO.popen(env, [reference, "--enable-frozen-string-literal", source], &:read)
    abort "process-clock-test: FAIL (CRuby oracle)" unless $?.success? && expected.start_with?("ns=#{ns}\n")
    if ns == -1 && expected != File.binread(source + ".expected")
      abort "process-clock-test: FAIL (recorded CRuby output)"
    end
    binaries.each do |mode, binary|
      [false, true].each do |stress|
        actual = IO.popen(env.merge("SPINEL_GC_STRESS" => stress ? "1" : nil), [binary], &:read)
        abort "process-clock-test: FAIL (#{ns}, #{mode}, stress=#{stress})" unless $?.success? && actual == expected
        count += 1
      end
    end
  end
  # Milliseconds and microseconds fit long after nanoseconds exceed int64.
  # Include the actual current timespec, both sides of the UUID timestamp wrap,
  # and the positive int64 microsecond boundary without replacing Ruby methods.
  source = root + "/tools/fixtures/clock_integer_units.rb"
  binaries = []
  [["default", []], ["promote", ["--int-overflow=promote"]], ["debug", ["--debug"]]].each do |mode, flags|
    binary = dir + "/units-" + mode
    log = dir + "/units-" + mode + ".log"
    unless system(compiler, *flags, source, "-o", binary, out: log, err: log)
      abort "process-clock-test: FAIL (units #{mode} compilation; #{File.read(log).lines.first})"
    end
    binaries << [mode, binary]
  end
  now = Time.now
  [[now.to_i, now.nsec], [-1, 999_999_999], [0, 0], [0, 999], [0, 999_999],
   [9_223_372_036, 854_775_808], [281_474_976_710, 655_999_999],
   [281_474_976_710, 656_000_000], [281_474_976_710, 657_000_000],
   [1_125_899_906_842, 624_123_456], [-1_125_899_906_843, 375_876_544],
   [9_223_372_036_854, 775_807_999]].each do |sec, nsec|
    env = {darwin ? "DYLD_INSERT_LIBRARIES" : "LD_PRELOAD" => shim,
           "SPINEL_TEST_CLOCK_SEC" => sec.to_s, "SPINEL_TEST_CLOCK_NSEC" => nsec.to_s}
    expected = IO.popen(env, [reference, "--enable-frozen-string-literal", source], &:read)
    ms = sec * 1000 + nsec / 1_000_000
    us = sec * 1_000_000 + nsec / 1000
    unless $?.success? && expected.start_with?("ms=#{ms}\nus=#{us}\n")
      abort "process-clock-test: FAIL (wide CRuby oracle)"
    end
    binaries.each do |mode, binary|
      [false, true].each do |stress|
        actual = IO.popen(env.merge("SPINEL_GC_STRESS" => stress ? "1" : nil), [binary], &:read)
        unless $?.success? && actual == expected
          abort "process-clock-test: FAIL (units #{sec}:#{nsec}, #{mode}, stress=#{stress})"
        end
        count += 1
      end
    end
  end
  puts "process-clock-test: #{count} CRuby comparisons pass"
end
