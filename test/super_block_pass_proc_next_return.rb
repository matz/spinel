# super(a, &pr) and m(a, &pr) with a proc or lambda literal: a `next v`,
# and a `return v` or `break v` in a lambda, give v to the yield. The last
# expression of the body alone is not enough. Each case has its own parent
# method, so no other block widens the yield's type.
class P1; def m(a) = [yield(a)]; end
class C1 < P1; def m(a) = super(a, &->(x) { next "t3" if x == 1; 5 }); end
class P2; def m(a) = [yield(a)]; end
class C2 < P2; def m(a) = super(a, &->(x) { return "r3" if x == 1; 5 }); end
class P3; def m(a) = yield(a) + 1; end
class C3 < P3; def m(a) = super(a, &proc { |x| next 1.5 if x == 1; 5 }); end
class P4; def m(a) = yield(a) + 1; end
class C4 < P4; def m(a) = super(a, &->(x) { return 2.5 if x == 1; 5 }); end
class P5; def m(a) = "#{yield(a)}!"; end
class C5 < P5; def m(a) = super(a, &lambda { |x| next :l if x == 1; 5 }); end
class P6; def m(a) = "#{yield(a)}!"; end
class C6 < P6; def m(a) = super(a, &->(x) { break :b if x == 1; 5 }); end
class P7; def m(a) = [yield(a)]; end
class C7 < P7; def m(a) = super(a, &->(x) { if x == 1 then next "a" else next [1] end }); end
class P8; def m(a) = [yield(a)]; end
class C8 < P8; def m(a) = super(a, &->(x) { [1, 2].each { |y| next "inner" }; x }); end
class P9; def m(a) = [yield(a)]; end
class C9 < P9; def m(a) = super(a, &->(x) { [1, 2].each { |y| return "out" if x == 1 }; x }); end
class P10; def m(a) = [yield(a)]; end
class C10 < P10; def m(a) = super(a, &->(x) { next if x == 1; "s" }); end
class P11; def m(a) = [yield(a)]; end
class C11 < P11
  def m(a)
    super(a, &proc { |x| return "pr" if x == 1; 5 })
    :after
  end
end
class P12; def m(a) = [yield(a)]; end
class C12 < P12; def m(a) = super(a, &->(x) { while true; break; end; [1].each { break 9 }; x }); end
class P13; def m(a) = [yield(a)]; end
class C13 < P13; def m(a) = super(a, &proc { |x| break "pb" if x == 1; 5 }); end
p C13.new.m(2), (C13.new.m(1) rescue $!.class)
# a `next v` and a tail of different kinds, one of them nil
class P14; def m(a) = [yield(a)]; end
class C14 < P14; def m(a) = super(a, &->(x) { next 7 if x == 1; nil }); end
class P15; def m(a); n = 0; n += 1 if yield(a); n; end; end
class C15 < P15; def m(a) = super(a, &->(x) { next 7 if x == 1; nil }); end
class P16; def m(a) = (v = yield(a)) ? "t#{v}" : "f"; end
class C16 < P16; def m(a) = super(a, &->(x) { next nil if x == 1; 5 }); end
class P17; def m(a) = yield(a).to_s + "|"; end
class C17 < P17; def m(a) = super(a, &->(x) { next 1.5 if x == 1; nil }); end
[C1, C2, C3, C4, C5, C6, C7, C8, C9, C10, C11, C12, C14, C15, C16, C17].each do |k|
  p [k.name, k.new.m(1), k.new.m(2)]
end

class Q1; def m(a) = yield(a); end
p Q1.new.m(1, &->(x) { next "d" if x == 1; 5 }), Q1.new.m(2, &->(x) { next "d" if x == 1; 5 })
class Q2; def m(a) = yield(a); end
p Q2.new.m(1, &->(x) { return 2.5 if x == 1; 5 }), Q2.new.m(2, &->(x) { return 2.5 if x == 1; 5 })
class Q3; def m(a) = yield(a); end
p Q3.new.m(1, &proc { |x| next :s if x == 1; 5 })
def q4(a) = [yield(a)]
p q4(1, &->(x) { next 7 if x == 1; nil }), q4(2, &->(x) { next 7 if x == 1; nil })
def q5(a); n = 0; n += 1 if yield(a); n; end
p q5(1, &->(x) { next 7 if x == 1; nil }), q5(2, &->(x) { next 7 if x == 1; nil })
