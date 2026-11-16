# `Kernel.puts` names Kernel's method: a module the top level includes under a condition, with a method of
# that name, does not make the call uncertain.
module L
  def puts(*a) = Kernel.print("L:", *a, "\n")
end
include L if ARGV.empty?
Kernel.puts "x"
Kernel.print "y\n"
