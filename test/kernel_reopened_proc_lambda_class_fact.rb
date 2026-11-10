# A program that reopens Kernel makes it a module of its own, and a call on
# it goes the way of a program module's calls. Its `proc` and `lambda` are
# still the builtin's unless the program defines them: then the value is a
# Proc, beside an object of a class defining `[]` here. The program's own
# `Kernel.proc` answers what it returns.
class Table
  def [] = "t"
end
module Kernel
  def self.proc(&b) = Table.new
  def self.tag = :k
end
p [Kernel.lambda { 1 }, Table.new][0][]
p [Kernel::lambda { 2 }, Table.new][0][]
p [::Kernel.lambda { 3 }, Table.new][0][]
p [Kernel.lambda { 4 }, Table.new][1][]
p Kernel.proc { 5 }.class
p [Kernel.proc { 6 }, 7][0][]
p Kernel.tag
