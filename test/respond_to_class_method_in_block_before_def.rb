# A class-side respond_to? or defined? in a block that runs at once,
# before the singleton method's def, answers false; after it, true.
class C; end
class D < C; end
a = [1]
a.each do
  p C.respond_to?(:z)
  p defined?(C.z)
  p D.respond_to?(:z)
  p(C.respond_to?(:z) ? C.z : :none)
end
K = Class.new do
  p C.respond_to?(:z)
end
loop do
  p defined?(C.z)
  break
end
def C.z = 1
a.each do
  p C.respond_to?(:z)
  p defined?(C.z)
  p D.respond_to?(:z)
  p(C.respond_to?(:z) ? C.z : :none)
end

a.each { p C.respond_to?(:w), C.respond_to?(:w, true) }
class << C; private def w = 2; end
a.each { p C.respond_to?(:w), C.respond_to?(:w, true) }
