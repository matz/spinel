# A Class.new evaluated more than once whose superclass lays its instances
# out as a builtin or a builder does (a Struct, an exception, an Array
# subclass) keeps the one class the site makes. That class, held as a value
# beside its instances, is a Class: a call its instances answer does not
# take it for one of them.
S = Struct.new(:a)
class Err < StandardError
end
class L < Array
end
def mk_struct = Class.new(S) { def hi = "struct-hi" }
def mk_err = Class.new(Err) { def hi = "err-hi" }
def mk_list = Class.new(L) { def hi = "list-hi" }

def ask(v)
  v.hi
rescue NoMethodError
  :nomethod
end

k = mk_struct
p [k, k.new(1), mk_struct].map { |v| ask(v) }
e = mk_err
p [e, e.new("m")].map { |v| ask(v) }
l = mk_list
p [l, l.new].map { |v| ask(v) }
h = { cls: k, obj: k.new(2) }
h.each_value { |v| p(v.respond_to?(:hi) ? v.hi : :no) }
