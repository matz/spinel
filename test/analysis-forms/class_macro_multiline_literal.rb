# A module_eval template holding a string over two lines: the text pass
# refuses the call; the form
# parses the text the macro computes, the string as written.
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
