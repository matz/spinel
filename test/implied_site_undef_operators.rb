# A method undefined below the class that defines it is not the one an
# implied call reaches: a Comparable operator's <=> and a pattern's
# deconstruct are asked of the plan at their site, which sees the undef.
class K
  include Comparable
  def <=>(o); 0; end
end
class L < K
  undef_method :<=>
end
a = K.new
p(a < K.new)
p(a == K.new)
begin
  p(L.new < K.new)
rescue NoMethodError => e
  puts e.message
end

class P
  def deconstruct; [1, 2]; end
end
class Q < P
  undef_method :deconstruct
end
case P.new
in [x, y] then p [x, y]
end
case Q.new
in [x, y] then p [x, y]
else p :none
end

# an arm whose class undefines === calls it, and CRuby raises
class R1
  def ===(o); true; end
end
class R2 < R1
  undef_method :===
end
case 1
when R1.new then puts "R1"
end
begin
  case 1
  when R2.new then puts "R2"
  else puts "noR2"
  end
rescue NoMethodError => e
  puts e.message
end

# a conversion, a display and Comparable#== whose method is undefined here raise
class CA
  def to_int; 1; end
  def to_s; "ca"; end
  def inspect; "CA!"; end
  include Comparable
  def <=>(o); 0; end
end
class CB < CA
  undef_method :to_int, :to_s, :inspect, :<=>
end
p [10, 20, 30][CA.new]
puts "#{CA.new}"
begin
  p [10, 20, 30][CB.new]
rescue TypeError => e
  puts e.message
end
begin
  puts "#{CB.new}"
rescue NoMethodError => e
  puts e.message
end
begin
  puts CB.new
rescue NoMethodError => e
  puts e.message
end
begin
  p CB.new
rescue NoMethodError => e
  puts e.message
end
begin
  p(CB.new == CB.new)
rescue NoMethodError => e
  puts e.message
end
begin
  p(CB.new.clamp(CA.new, CA.new))
rescue NoMethodError => e
  puts e.message
end
