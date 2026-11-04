# A bare super in respond_to? passes the name and include_all on as they
# came in: an Integer that may be nil boxes, and a name that is not a
# Symbol or a String raises TypeError, as in CRuby.
class Rt
  def respond_to?(n, a = false) = super
  def hi = 1
  private def pv = 2
end

def kk(f) = f ? 1 : nil

begin
  Rt.new.respond_to?(kk(true))
rescue TypeError => e
  puts e.message
end
p Rt.new.respond_to?(:pv, kk(false))
p Rt.new.respond_to?(:pv, kk(true))
p Rt.new.respond_to?("hi")

class SS < String; end
p Rt.new.respond_to?(SS.new("hi"))

[:hi, "hi", nil, 1.5].each do |v|
  begin
    p Rt.new.respond_to?(v)
  rescue TypeError => e
    puts e.message
  end
end

# An explicit super(n, a) does the same.
class R2
  def respond_to?(n, a = false) = super(n, a)
  def hi = 1
end
p R2.new.respond_to?(:hi, kk(false)), R2.new.respond_to?(:x, kk(true))
begin
  R2.new.respond_to?(kk(true))
rescue TypeError => e
  puts e.message
end

# A block argument runs too.
$n = 0
def side
  $n += 1
  nil
end
class R3
  def respond_to?(n, a = false) = super(n, a, &side)
  def hi = 1
end
p R3.new.respond_to?(:hi), $n
