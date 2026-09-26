# Class#subclasses leaves out the singleton class a `def obj.m` gives an
# object, whether the class is named directly or read out of a boxed slot.
class Base; end
class A < Base; end
class B < Base; end
b = Base.new
def b.hi = 1
p b.hi
p Base.subclasses.sort_by(&:name)
c = [Base, 0][0]
p c.subclasses.sort_by(&:name)
