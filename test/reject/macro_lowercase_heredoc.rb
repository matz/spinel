# A heredoc argument opened as <<eos has its body outside the call's range:
# the call is not expanded (its run-time module_eval is refused), and the
# body is not left behind as code.
module Consts
  def named(name, body) = module_eval("def self.#{name} = #{body}")
end
class Calc
  extend Consts
  named :hello, <<eos
"hi"
eos
end
p Calc.hello
