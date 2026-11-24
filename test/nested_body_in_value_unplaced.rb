# A class body in a value that may not run with its statement (behind a condition, a rescue modifier, a block,
# a lambda, a safe navigation, an interpolation, a `defined?`) is not placed at the statement: each of these
# answers as CRuby does with the body's def defined where it ran.
class K
  def m = 0
end
f = false

z = f && (class K; def m = 1; end; 0)
p K.new.m
z = (class K; def m = 2; end; 0) if f
p K.new.m
z = f ? (class K; def m = 3; end; 0) : 0
p K.new.m
z = [1].each { class K; def m = 5; end }
p K.new.m
l = -> { class K; def m = 6; end }
p K.new.m
l.call
p K.new.m
z = ((class K; def m = 10; end; 0) rescue 1)
p K.new.m
