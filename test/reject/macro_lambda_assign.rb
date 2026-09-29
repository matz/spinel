# A define_method lambda that assigns a local it captured from the macro: its
# later reads are not the value the macro passed in, so the macro is left as
# written (and its run-time define_method refused).
module Consts
  def reader(name) = define_singleton_method(name, -> { name = name.to_s; name.upcase })
end
class Calc
  extend Consts
  reader :k
end
p Calc.k
