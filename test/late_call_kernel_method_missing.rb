# A method_missing defined in Kernel (among Object's methods, which a
# program class's lookup ends in) answers a call that runs before the
# method's first def, on an instance and on a class
module Kernel
  def method_missing(n, *a) = :kmm
end
class E; end
def b_inst = E.new.o1
p b_inst
class E; def o1 = 1; end
p b_inst

def b_cls = E.o2
p b_cls
def E.o2 = 2
p b_cls
