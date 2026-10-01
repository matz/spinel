# A class variable handed to a parameter that takes its value boxed and
# appends to it: the box holds a copy of the class variable's String, so
# the append would not reach it. Refused rather than compiled with the
# append lost.
def poly(io) = (io << "y"; nil)
poly([]) if ARGV.size > 5   # a second caller makes io POLY
class Y
  @@d = String.new
  def self.go = (poly(@@d); @@d)
end
p Y.go
