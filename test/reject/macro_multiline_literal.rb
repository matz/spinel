# A module_eval template holding a string over two lines is not joined onto
# the call's line (that would change the string): the call is left as
# written, and the run-time module_eval is refused.
module Consts
  def greeting(name)
    module_eval <<-RUBY, __FILE__, __LINE__ + 1
      def self.#{name} = "a
b"
    RUBY
  end
end
class Calc
  extend Consts
  greeting :hello
end
p Calc.hello
