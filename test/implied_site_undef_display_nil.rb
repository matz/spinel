# A display whose to_s or inspect the record proves undefined where it runs
# raises for an instance, and a nil operand keeps nil's own display (the empty
# String, "nil"), which CRuby never raises. The operand runs once.
class C
  def to_s; "c"; end
  def inspect; "ci"; end
end
class C
  undef_method :to_s, :inspect
end
$runs = 0
def made; $runs += 1; C.new; end
none = ARGV.size > 5 ? C.new : nil
puts none
print none, "\n"
puts "[#{none}]"
p none
p [none]
inst = C.new
begin; puts inst; rescue NoMethodError => e; puts "puts #{e.class}"; end
begin; print inst; rescue NoMethodError => e; puts "print #{e.class}"; end
begin; puts "[#{inst}]"; rescue NoMethodError => e; puts "interpolation #{e.class}"; end
begin; p inst; rescue NoMethodError => e; puts "p #{e.class}"; end
begin; p made; rescue NoMethodError => e; puts "p made #{e.class}"; end
puts $runs
