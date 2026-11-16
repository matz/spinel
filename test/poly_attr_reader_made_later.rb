# an attr_reader made only after the first calls: NoMethodError before it
class A
  def initialize; @x = 7; end
end
class B
  def x; 2; end
end
xs = [A.new, B.new]
xs.each do |o|
  begin
    p o.x
  rescue NoMethodError => e
    p e.class
  end
end
class A
  attr_reader :x
end
xs.each { |o| p o.x }
