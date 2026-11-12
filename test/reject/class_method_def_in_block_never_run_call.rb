# A def in a block that never runs defines nothing, so a call of the class
# method it alone would have defined is a call of a method the program
# defines nowhere: refused when the program is built, as `A.cm` is with no
# def at all, or with the def under `if false`.
# spinel: reject-unsupported: no class in the program defines a class method 'cm'
class A
  def self.skip; end
  skip { def self.cm = 2 }
end
begin
  p A.cm
rescue NoMethodError => e
  puts e.message
end
