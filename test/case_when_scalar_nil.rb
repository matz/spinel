# `case` on an Integer or Float slot that holds nil (here from a method only
# nil is passed to, which the nullable-value analysis does not mark): a
# `when Integer` arm is Integer === nil, false, and `when nil` matches only
# nil, never a 0. Statement and value forms.
def f(v) = v

z = 1
z = f(nil) if ARGV.empty?
case z
when Integer then p :int
when nil then p :nil
end
r = case z
    when Numeric then :num
    when NilClass then :nilclass
    end
p r
p(case z when Object then :obj end)

w = 1.5
w = f(nil) if ARGV.empty?
case w
when Float then p :flt
when NilClass then p :nilc
end
p(case w when Comparable then :cmp else :other end)

k = 0
case k
when nil then p :nil
when Integer then p :int0
end
p(case k when nil then :nil else :zero end)
x = 0.0
p(case x when nil then :nil when Float then :f0 end)
n = 7
p(case n when Integer then :i when nil then :n end)
