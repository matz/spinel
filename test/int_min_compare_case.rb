# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Comparisons, case/when and pattern matching with a -2**63 subject.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
p m == m
p m == -9223372036854775808
p m != 0
p m < 0
p m <= m
p m > -9223372036854775807
p m >= 0
p m <=> 0
p 0 <=> m
p m <=> m
p m.between?(m, 0)
p m.clamp(m, 0)
p m.clamp(0, 5)
p [m, 0].min
p [0, m].max
p m.eql?(-9223372036854775808)
p m.equal?(m)
p m === m
p Integer === m
p Comparable === m
p NilClass === m
case m
when nil then puts "nil"
when 0 then puts "zero"
when ..-1 then puts "negative"
else puts "other"
end
case m
when Integer then puts "int"
else puts "else"
end
r = case m
    in nil then "nil"
    in Integer => v if v < 0 then "neg #{v}"
    in Integer then "int"
    end
puts r
case m
in ..0 then puts "in-neg"
else puts "in-else"
end
p(m.zero? ? "z" : "nz")
p m.positive?
p m.negative?
p m.nonzero?
p (m..0).cover?(m)
p (m..0).include?(m)
p (m...0).cover?(m)
