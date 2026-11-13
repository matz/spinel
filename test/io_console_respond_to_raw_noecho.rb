# After require "io/console", an IO answers respond_to? true for the
# console methods Spinel compiles (raw, noecho, winsize, winsize=).
require "io/console"
p $stdout.respond_to?(:raw), $stdout.respond_to?(:noecho)
p $stdin.respond_to?(:raw), $stdin.respond_to?(:noecho)
p $stdout.respond_to?(:winsize), $stdout.respond_to?(:winsize=)
p STDERR.respond_to?(:raw), STDERR.respond_to?("noecho")
f = File.open(__FILE__)
p f.respond_to?(:raw), f.respond_to?(:noecho)
f.close
m = [:raw, :noecho, :winsize, :console_size][rand(1)]
p $stdout.respond_to?(m)
%i[raw noecho winsize console_size].each { |n| p [n, $stdout.respond_to?(n)] }
x = [$stdout, 1, "s"].first
p x.respond_to?(:raw), x.respond_to?(:noecho)
p $stdout.respond_to?(:console_size), $stdout.respond_to?(:raw, true)
