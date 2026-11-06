# Under the load graph a class-body macro whose body runs const_set writes
# the constant where the call stands, and the load record keys it as any
# written constant: two classes' SIZE stay apart from each other and from
# a top-level SIZE, and a read before the call does not see it. A macro
# that reads per-class state an earlier one wrote (`kind :box`) follows it
# over the class's statements, its reopened bodies and both arms of an if.
module Consts
  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end

  def constant(c, v = nil) = const_set(c, v || public_send("calc_#{kind}_#{c.to_s.downcase}"))
  def tag(c, text) = const_set(c, text.inspect)
end

class Calc
  extend Consts
  EARLY = defined?(SIZE) ? SIZE : :none
  def self.calc_box_size = 32
  kind :box
  constant :SIZE
  def self.info = [EARLY, SIZE]
end

class Small
  extend Consts
  constant :SIZE, 4
end
SIZE = 1
p Calc.info, Small::SIZE, SIZE, Calc.kind

# the state lasts across a reopened body, and arms that agree keep it
class Calc
  if SIZE > 0
    kind :wide
  else
    kind :wide
  end
  def self.calc_wide_width = 80
  constant :WIDTH
end
p Calc::WIDTH, Calc.kind

# a value the macro computes is spelled as CRuby's inspect answers it
class Tagged
  extend Consts
  tag :ESC, "a\e#b"
  tag :PLAIN, "ab"
end
p Tagged::ESC, Tagged::PLAIN
