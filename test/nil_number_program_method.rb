# An Integer or Float that can be nil (`x = f ? 7 : nil`) runs nil's own
# method where the program gives nil one, as CRuby does: NilClass's, or
# Object's where the number's class has its own. The number's call
# unwrapped the value (NoMethodError for nil), or ran Object's method with
# nil as self. Active Support has this shape: `blank?` and `to_param` are
# defined on Object, NilClass and Numeric.
class NilClass
  def label = "none"
  def blank? = true
  def shown = "nil"
  def note = puts("nil note")
end

class Integer
  def label = "int #{self}"
  def blank? = false
  def desc = "integer #{self}"
  def note = puts("int note #{self}")
end

class Float
  def label = "float #{self}"
  def desc = "float #{self}"
end

class Object
  def shown = "obj"
  def desc = "object #{inspect}"
end

def pick(f) = f ? 7 : nil
def pickf(f) = f ? 1.5 : nil

[true, false].each do |f|
  x = pick(f)
  p x.label, x.blank?, x.shown, x.desc
  x.note
  y = pickf(f)
  p y.label, y.desc
end
