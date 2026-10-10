# A nullable Integer or Float local carries its nil beside the value (the
# sp_oint / sp_ofloat flag), and nil? / == nil / inspect all honour it. These
# reads decided from the slot's kind instead: a case subject took `when
# Integer` and missed `when nil` and `in nil`, `Integer === x` and
# `x.eql?(nil)` folded, a Hash keyed by it printed -9223372036854775808,
# Kernel#String / Array / Integer and print read the number, and a Float
# global written from it printed NaN. Each value is read nil and present.
ints = [5, 6]
flts = [1.5, 2.5]
[ARGV.size + 7, ARGV.size].each do |i|
  b = ints[i]
  f = flts[i]
  s = case b
      when nil then "nil"
      when Integer then "int"
      end
  t = case f
      when Float then "flt"
      when NilClass then "nil"
      end
  case b
  when ..0 then puts "b neg"
  when Comparable then puts "b cmp #{s}"
  else puts "b else #{s}"
  end
  case f
  in nil then puts "f in-nil #{t}"
  in Numeric then puts "f in-num #{t}"
  end
  r = case b
      in Integer | Float then "in-num"
      in ..0 then "in-neg"
      else "in-else"
      end
  puts r
  p [Integer === b, NilClass === b, Numeric === f, Float === f, Comparable === b]
  p [b === nil, f === nil, b.eql?(nil), f.equal?(nil), b <=> nil, f <=> nil]
  p [b.respond_to?(:succ), f.respond_to?(:floor), b.object_id == nil.object_id]
  p({ b => 1 }, { 2 => b }, [b, b].tally)
  p [String(b), String(f), Array(b), Array(f), [*b, 3], [*f]]
  begin
    p Integer(b)
  rescue TypeError => e
    puts "TypeError #{e.message}"
  end
  begin
    p Float(f)
  rescue TypeError => e
    puts "TypeError #{e.message}"
  end
  print b, "|", f, "\n"
  g = flts[i]
  p f == g, f != g
  $nsr = f
  p $nsr
end
