# A `def S.new` in a method nothing calls is a version that never runs: the call of the Struct's
# `new` the class body defines binds that def. S's constant is written twice (the builder, then
# `class S`), so the receiver names no class the record holds final.
S = Struct.new(:a)
class S
  def self.new(x) = allocate.tap { |o| o.a = x + 1 }
end
module Late
  def self.h(x) = S.new(x).a
  def self.d
    def S.new(x) = allocate.tap { |o| o.a = x * 10 }
  end
end
p Late.h(4)
p S.new(1).a
