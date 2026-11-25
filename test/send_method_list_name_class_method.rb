# public_send and send of a name read from a method list in a class
# method, as a test runner lists its tests.
class Suite
  def self.tests = public_instance_methods(true).grep(/\Atest_/).sort
  def self.test_names = tests.map(&:to_s)
end

class Mine < Suite
  def test_one = puts("one ran")
  def test_two = puts("two ran")
end

Mine.tests.each { |name| Mine.new.public_send name }
[Mine].each do |k|
  k.test_names.each { |name| k.new.send name }
end

# beside a class with method_missing, with to_sym at the send
class Mock
  def method_missing(name)
  end
end

[Mine].each do |k|
  k.test_names.each { |name| k.new.public_send name.to_sym }
end

# an attr writer that a class method's list returns
class Box < Suite
  attr_writer :xa
  def xa_get = @xa
  def self.writers = public_instance_methods(true).grep(/\Ax.*=\z/)
end

b = Box.new
Box.writers.each { |n| b.public_send(n, 9) }
p b.xa_get

# an attr writer that a list on a constant returns
class Pair
  attr_accessor :left
end

pr = Pair.new
Pair.public_instance_methods(false).sort.each { |n| pr.public_send(n, 3) if n.to_s.end_with?("=") }
p pr.left
