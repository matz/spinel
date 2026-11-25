# public_send and send of a name read from a method list that a method
# returns for a class held in a variable.
class T
  def test_a = puts("a ran")
  def test_b = puts("b ran")
end

class U
  def other = 1
end

def names(klass) = klass.public_instance_methods(false)
names(T).sort.each { |name| T.new.public_send name }
p :to_s.size

# a name the class lacks raises NoMethodError
begin
  names(T).sort.each { |name| U.new.public_send name }
rescue NoMethodError => e
  puts e.message
end

# attr readers and writers that a method list returns
class Point
  attr_accessor :value
  def initialize = @value = 1
end

pt = Point.new
names(Point).sort.each do |name|
  if name.to_s.end_with?("=")
    pt.public_send(name, 5)
  else
    p pt.public_send(name)
  end
end
p pt.value
