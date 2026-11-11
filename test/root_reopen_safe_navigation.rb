# `&.` on nil answers nil without calling a program's Object or NilClass
# method; on a receiver that is not nil the method runs.
class Object; def foo = 1; def each_thing; yield 1; 2; end; end
class NilClass; def nn = 5; end
p nil&.foo
x = nil
p x&.foo
p x&.nn
s = [nil, "a"].first
p s&.foo
p s&.each_thing { break 9 }
t = ["a", nil].first
p t&.each_thing { |v| v + 1 }
p t&.foo
p nil&.each_thing { break 9 }
y = 3
p y&.foo
p y&.each_thing { break 7 }
