# Compiled with --defer-refusals: an output call that ends a `begin` body whose value is dropped (a tail
# position, which emits through the output call directly) is refused like any bare call of a name a
# conditional include defines, and raises NotImplementedError where it stands.
# spinel: defer-refusals: 1:start
# spinel: defer-message: a call of a name that a mixin of main or Object defines
$stdout.write("start\n")
module L
  def puts(*a) = $stdout.write("L:#{a.join}\n")
end
include L if ARGV.empty?
r = begin
  puts "a"
end
