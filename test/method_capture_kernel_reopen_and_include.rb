# A Method made of a Kernel function binds what the lookup meets on main at the capture: main's singleton, then Object's
# chain with the modules in it. A program's reopening of Kernel (#puts), or a module the top level includes, is that
# method, whether main gets a def of the name after the capture or not; the wrapper that stands for Kernel's own never
# calls past it.
module Kernel
  def puts(*a) = $stdout.write("K:#{a.join}\n")
end
m = method(:puts)
m.call("x")
def self.puts(*a) = $stdout.write("main\n")
m.call("y")
puts "z"

module Loud
  def print(*a) = $stdout.write("L:#{a.join}\n")
end
include Loud
pr = method(:print)
def self.print(*a) = $stdout.write("main print\n")
pr.call("w")
