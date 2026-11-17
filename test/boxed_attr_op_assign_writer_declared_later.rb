# A boxed `recv.attr op= v` on a class with a reader and no writer raises NoMethodError
# for `attr=`, and the writer the class declares after the first call answers the later calls.

class A
  attr_accessor :n
  def initialize = @n = 1
end
class B
  attr_reader :n
  def initialize = @n = 10
end
box = [A.new, B.new]
box.each do |o|
  begin
    o.n += 1
    p o.n
  rescue NoMethodError => e
    p e.class
  end
end
class B
  attr_writer :n
end
box.each { |o| o.n += 5; p o.n }
