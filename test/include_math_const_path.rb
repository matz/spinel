# A path through a class or module that includes Math reads Math's
# constants: Klass::PI as a bare PI inside it does.
module Calc
  include Math
  def self.tau = PI * 2
end
class Base
  include Math
end
class Sub < Base; end
module Mid
  include Math
end
class Deep
  include Mid
end
class Plain; end

p Calc::PI, Calc::E, Calc.tau
p Base::PI, Sub::E, Mid::PI, Deep::E
p Calc::PI * 2 == Math::PI * 2
p defined?(Calc::PI), defined?(Sub::E), defined?(Deep::PI), defined?(Plain::PI), defined?(Calc::NOPE)
p(defined?(Calc::PI) ? Calc::PI : 0)
begin
  p Plain::PI
rescue NameError => e
  puts e.message
end
