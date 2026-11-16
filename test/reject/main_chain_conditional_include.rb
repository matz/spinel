# A bare call of a name that a module the top level includes under a condition defines is refused: CRuby finds the
# module's method only if the include ran, which the compiler cannot tell. (Under --defer-refusals the statement raises
# NotImplementedError where it is evaluated.)
module L
  def puts(*a) = $stdout.write("L:#{a.join}\n")
end
include L if ARGV.empty?
puts "a"
