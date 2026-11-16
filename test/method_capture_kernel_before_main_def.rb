# A Method made of a Kernel function binds Kernel's, as CRuby's lookup finds it at the capture. A `def self.puts` that main
# gets after the capture (or removed before it) is not the Method's: calling it prints Kernel's output, while a bare
# `puts` after the def reaches main's own.
m = method(:puts)
def self.puts(*a) = Kernel.puts("mine", *a)
m.call("x")
puts "y"

def self.print(*a) = Kernel.puts("own print", *a)
class << self
  remove_method :print
end
pr = method(:print)
pr.call("z\n")
puts m.owner
