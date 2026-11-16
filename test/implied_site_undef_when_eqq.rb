# A when whose pattern's class undefines === where the record places the when
# raises, whatever == the class has: Object#=== (rb_equal's ==) is the answer
# only for a class that never had its own === undefined.
class Matcher
  def ==(o); true; end
  def ===(o); true; end
end
class Other; end
puts((case 1; when Matcher.new then "matched before"; end))
class Matcher
  undef_method :===
end
m = Matcher.new
arr = [1]
begin
  case arr
  when m then puts "matched"
  else puts "else"
  end
rescue NoMethodError => e
  puts e.class
end
hsh = {a: 1}
begin
  case hsh
  when m then puts "matched"
  else puts "else"
  end
rescue NoMethodError => e
  puts e.class
end
begin
  case 1
  when m then puts "matched"
  else puts "else"
  end
rescue NoMethodError => e
  puts e.class
end
boxed = [Matcher.new, Other.new][0]
begin
  case 1
  when boxed then puts "matched"
  else puts "else"
  end
rescue NoMethodError => e
  puts e.class
end
plain = [Matcher.new, Other.new][1]
case 1
when plain then puts "matched"
else puts "else"
end
