# A boxed value given to Complex() alone that is no number is CRuby's
# "can't convert X into Complex" (one of two is "not a real"); an unparsable
# String names itself; Kernel#Float converts a boxed Complex with an exact
# zero imaginary part and a Time; and a Complex whose imaginary part is 0.0
# is a member of a Float range, as Complex#<=> compares it.

def try
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end

class Thing; end
vals = [:a, true, false, [1], {k: 1}, Thing.new, "abc", nil, 1]
vals.each do |v|
  try { Complex(v) }
  try { Complex(v, 1) }
end

zs = [Complex(2, 0), Complex(2, 1), Complex(2, 0.0), Time.at(3, 500000), 1]
zs.each { |z| try { Float(z) } }
zs.each { |z| try { (1.0..3.0) === z } }
zs.each { |z| try { (1.0..3.0).cover?(z) } }
zs.each do |z|
  try do
    case z
    when 1.0..3.0 then :in
    else :out
    end
  end
end
