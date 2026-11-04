# Range#step and #overlap? over a Range of Times: step walks n seconds at a
# time (an endless one until the block breaks), and overlap? answers as
# CRuby's range_overlap.
a = Time.utc(2026, 1, 1); b = a + 7200
p (a..b).step(3600).map(&:hour)
p (a...b).step(3600).to_a.size
p (a..b).step(0).to_a
p (b..a).step(3600).to_a
p (a..b).step(1.5).first(2)
(a..b).step(3600) { |t| puts t.hour }
(a..).step(1800) { |t| break if t.hour >= 1; puts t.min }
r = a..b
begin
  (..b).step(3600) { |t| p t }
rescue ArgumentError => e
  puts e.message
end
x = r.step(3600) { |t| }
p x
tt = ->(d) { Time.utc(2026, 1, d) }
p (tt[1]..tt[5]).overlap?(tt[5]..tt[9]), (tt[1]...tt[5]).overlap?(tt[5]..tt[9]), (tt[1]..tt[5]).overlap?(tt[6]..tt[9])
p (tt[1]..).overlap?(..tt[1]), (tt[1]...).overlap?(...tt[1]), (..tt[3]).overlap?(..tt[1]), (tt[5]..tt[1]).overlap?(tt[1]..tt[9])
p (tt[2]..tt[2]).overlap?(tt[2]...tt[2]), (tt[2]..tt[4]).overlap?(tt[2]..tt[4])
h = { r: tt[1]..tt[5] }
p h[:r].overlap?(tt[3]..tt[4])
p (tt[1]..tt[5]).overlap?(1..2)
