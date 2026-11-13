# Float#round(n) on a boxed Float rounds as the typed Float does: a tie
# the scaled product fell short of (1.005 * 100 is 100.49999999999999)
# still rounds up, so one number does not round two ways in one program.
def as_float(v)
  case v
  when Float then v
  when String then v.to_f
  else 0.0
  end
end

x = "1.005".to_f
p x.round(2)
p as_float(x).round(2)
p as_float("0.285").round(2)
p as_float("-1.005").round(2)
p as_float("2.675").round(2)
p as_float("1.15").round(1)
p as_float("0.0001").round(2)
p as_float("-0.0001").round(2)
p as_float("1.23456789012345678").round(16)
p as_float("1.5").round(400)
p as_float("1234.5").round(-2)
[1.005, 2, "x".size * 0.125].each { |v| p v.round(2) }
