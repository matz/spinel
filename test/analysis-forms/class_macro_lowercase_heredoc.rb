# A heredoc argument opened as <<eos: the text pass refuses the call; the form computes the text the
# macro passes module_eval and grafts it where the call stands.
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
