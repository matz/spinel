# String.try_convert calls a user object's #to_str, private or not, as
# CRuby's rb_check_string_type does: its String answer, or nil for an
# object without one; a String answers itself, nil answers nil.
class X
  def to_str = "a"
end
class Y
  private def to_str = "b"
end
class Z
end
p String.try_convert(X.new), String.try_convert(Y.new), String.try_convert(Z.new), String.try_convert("s"), String.try_convert(nil)
v = [X.new, 1].first
p String.try_convert(v)
