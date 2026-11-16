# Compiled with --defer-refusals: a bare call of a name that a module the top level includes under a condition
# defines (the compiler cannot tell the include ran, CRuby prints `L:a` when it did) is refused, and raises
# NotImplementedError where it stands, after the statements ahead of it. Without the flag it is refused.
# spinel: defer-refusals: 1:start
# spinel: defer-message: a call of a name that a mixin of main or Object defines
$stdout.write("start\n")
module L
  def puts(*a) = $stdout.write("L:#{a.join}\n")
end
include L if ARGV.empty?
puts "a"
