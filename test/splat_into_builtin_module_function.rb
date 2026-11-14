# A splat into a builtin module's or class's own method (Math.sqrt(*a),
# Integer.sqrt(*a)) spreads its elements, with the arity error past the counts.
a = [16]
p Math.sqrt(*a)
def m(*a) = Math.sqrt(*a)
p m(16)
def m2(*a) = Math.atan2(*a)
p m2(1.0, 1.0)
def m3(*a) = Integer.sqrt(*a)
p m3(16)
p Math.send(:sqrt, *a)
def mm(*a) = Math.sqrt(*a)
begin
  mm(1, 2)
rescue ArgumentError => e
  p e.message
end
RealMath = Math
def rs(*args) = RealMath.send(:sqrt, *args)
p rs(25)
def hy(*a) = Math.hypot(*a)
p hy(3, 4)
def lg(*a) = Math.log(*a)
p lg(8, 2), lg(1)
