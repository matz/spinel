# A Method of a Kernel function made before a def of the name, or in a block run at once on main, or of a conversion
# main redefines later, is Kernel's. `Kernel.puts` in a top-level def is Kernel's whatever self is, beside main's own def.
m = method(:puts)
def puts(*a) = Kernel.print("objputs ", *a, "\n")
m.call("x")
puts "y"

ms = []
[1].each { ms << method(:print) }
def self.print(*a) = Kernel.puts("main print", *a)
ms[0].call("b\n")
print "c"

mi = method(:Integer)
mp = method(:p)
def self.Integer(x) = :mine
def self.p(*a) = Kernel.puts("mine")
Kernel.p mi.call("12")
mp.call(5)
p 6

def self.format(*a) = "mine"
def helper = Kernel.format("%03d", 7)
Kernel.p helper
Kernel.p format("%d", 1)
def self.puts(*a) = Kernel.print("main ", *a, "\n")
def helper2 = Kernel.puts("h")
helper2
Object.new.send(:helper2)
