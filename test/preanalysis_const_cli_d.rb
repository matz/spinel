# spinel: not-cruby -- exercises Spinel CLI -D compile-time constants and --target=wasm32-wasi pruning
require "tmpdir"

dir = File.join(Dir.tmpdir, "spinel_pre_const_cli_#{Process.pid}")
Dir.mkdir(dir)
begin
  cli_rb = File.join(dir, "cli_d.rb")
  File.write(cli_rb, <<~'RUBY')
    TARGET = "default" unless defined?(TARGET)
    if FEATURE_X && TARGET == "ios" && LEVEL >= 2 && !FLAG && NOTHING.nil?
      puts "cli_d_ok"
    else
      require "spinel_nonexistent_server_gem"
      dead_cli = "never"
    end
    p [FEATURE_X, TARGET, LEVEL, FLAG, NOTHING, dead_cli, defined?(dead_cli)]
  RUBY

  %w[graph inline].each do |model|
    bin = File.join(dir, "cli_d_#{model}")
    ok = system("bin/spinel", "--load-model=#{model}", "--strict-require",
                "-D", "FEATURE_X", "-D", "TARGET=ios", "-D", "LEVEL=2",
                "-D", "FLAG=false", "-D", "NOTHING=nil", "-o", bin, cli_rb,
                out: File::NULL, err: File::NULL)
    puts "model=#{model} build=#{ok}"
    puts IO.popen([bin], &:read)
  end

  # Last -D definition wins
  last_rb = File.join(dir, "last.rb")
  File.write(last_rb, "puts(MODE == 'b' ? 'last_wins' : 'wrong')\n")
  last_bin = File.join(dir, "last_bin")
  system("bin/spinel", "-D", "MODE=a", "-D", "MODE=b", "-o", last_bin, last_rb,
         out: File::NULL, err: File::NULL)
  puts IO.popen([last_bin], &:read)

  # User assignment shadows -D constant instead of folding against CLI value
  shadow_rb = File.join(dir, "shadow.rb")
  File.write(shadow_rb, <<~'RUBY')
    FOO = "override"
    puts(FOO == "cli" ? "wrong" : "shadowed:#{FOO}")
  RUBY
  shadow_bin = File.join(dir, "shadow_bin")
  system("bin/spinel", "-D", "FOO=cli", "-o", shadow_bin, shadow_rb,
         out: File::NULL, err: File::NULL)
  puts IO.popen([shadow_bin], &:read)

  # --target=wasm32-wasi prunes server-only else branch before require resolution
  wasi_rb = File.join(dir, "wasi.rb")
  File.write(wasi_rb, <<~'RUBY')
    if RUBY_PLATFORM.include?("wasi")
      puts "on_wasi"
    else
      require "spinel_nonexistent_pg"
      puts "on_server"
    end
  RUBY
  lg = IO.popen(["bin/spinel", "--target=wasm32-wasi", "--strict-require",
                 "--dump-load-graph", wasi_rb], &:read)
  puts lg.lines.first.to_s.strip

  # Invalid -D specifications are refused at compile time
  %w[lowercase RUBY_ENGINE=other 123=1 =val].each do |bad|
    ok = system("bin/spinel", "-D", bad, "-S", "-o", File.join(dir, "bad.c"), wasi_rb,
                out: File::NULL, err: File::NULL)
    puts "reject=#{bad}:#{!ok}"
  end
ensure
  Dir.children(dir).each { |f| File.delete(File.join(dir, f)) }
  Dir.rmdir(dir)
end
