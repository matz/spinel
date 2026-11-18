# const_defined? with a name known only at run time on an anonymous module a
# constant holds: `V = Module.new` and `W = Module.new.freeze` (a loader's
# constant-name validator). The module has no constants of its own, so asked
# for its own alone it answers false; asked with inheritance it answers for
# the top-level names, as a module with a body does. The frozen one stays
# frozen.

V = Module.new
W = Module.new.freeze

module Named
  Inner = 1
end

names = ARGV.empty? ? %w[Inner String Nope] : ARGV
names.each do |n|
  p [n, V.const_defined?(n, false), V.const_defined?(n)]
  p [n, W.const_defined?(n, false), W.const_defined?(n)]
  p [n, Named.const_defined?(n, false), Named.const_defined?(n)]
end
p W.frozen?
p V.frozen?
p W.const_defined?(:Inner, false)
