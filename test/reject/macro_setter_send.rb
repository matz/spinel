# public_send of a setter name in a macro is not rewritten to `attr=(v)`,
# which would read as a local assignment: the call is left as written (a
# run-time public_send, refused).
module Setters
  def set(attr, v) = public_send("#{attr}=", v)
end
class Calc
  extend Setters
  class << self
    attr_accessor :size
  end
  set :size, 3
end
p Calc.size
