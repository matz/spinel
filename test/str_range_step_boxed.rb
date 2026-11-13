# A boxed String Range answers step and % with no block, as a typed one
# does, and a boxed stride that is no number raises the TypeError of CRuby
def t
  yield
rescue => e
  "#{e.class}: #{e.message}"
end
box = [("a".."e"), ("a"..), 2, nil, :s, true]
sr = box[0]; esr = box[1]; two = box[2]
p t { sr.step(2).to_a }
p t { sr.step(two).to_a }
p t { sr.step.first(2) }
p t { sr.step(2).inspect }
p t { (sr % 2).to_a }
p t { (sr % two).inspect }
p t { esr.step(2).first(3) }
a = []
sr.step(2) { |x| a << x }
p a
# the stride, boxed, on a boxed and on a typed receiver
p t { sr.step(box[3]).to_a }
p t { sr.step(box[4]).to_a }
p t { (sr % box[5]).to_a }
p t { ("a".."e").step(box[3]).to_a }
p t { ("a".."e").step(box[4]).to_a }
p t { (("a".."e") % box[3]).to_a }
# every Range kind through one call site
[("a".."c"), (1..5), (1.0..3.0)].each { |r| p t { r.step(2).to_a } }
# a Range of Symbols keeps its members Symbols
sbox = [:a, :e, 2, nil, :s, 1.5]
yr = (sbox[0]..sbox[1]); eyr = (sbox[0]..)
p t { yr.step(2).to_a }
p t { yr.step(sbox[2]).to_a }
p t { yr.step.first(2) }
p t { yr.step(2).inspect }
p t { (yr % 2).to_a }
p t { (yr % 2).inspect }
p t { eyr.step(2).first(3) }
p t { eyr.each.first(2) }
b = []
yr.step(2) { |x| b << x }
p b
p t { yr.step(sbox[3]).to_a }
p t { yr.step(sbox[4]).to_a }
p t { yr.step(sbox[5]).first(1) }
