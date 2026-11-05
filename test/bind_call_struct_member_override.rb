# `S.instance_method(:d).bind_call(obj)` for a Struct member d binds S's
# member, also when obj's class is a subclass that overrides d or d=:
# CRuby answers the member, and the subclass's own instance_method answers
# its def. The member's bind_call helper read the member through a call of
# d, which the subclass's def answered.
S = Struct.new(:d, :f)
class V < S
  def d = "own #{super}"
  def f=(v)
    super(v * 2)
  end
end
v = V.new(13, 0)
p S.instance_method(:d).bind_call(v), V.instance_method(:d).bind_call(v)
p S.instance_method(:f=).bind_call(v, 14), v.f
p V.instance_method(:f=).bind_call(v, 15), v.f
p v.d
