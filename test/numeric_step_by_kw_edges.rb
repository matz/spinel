# step(by:) on a Float, or with a Float step, is endless as the Integer one is;
# by: nil is the default step; a program's own step(by:) keeps its keywords.
1.0.step(by: 0.5) { |x| break if x > 2; p x }
1.step(by: 0.5) { |x| break if x > 2; p x }
1.0.step(by: -0.5) { |x| break if x < 0; p x }
1.step(by: nil) { |i| break if i > 3; p i }
1.step(to: 3, by: nil) { |i| p i }

class Counter
  def step(by:, to: 0) = "user step #{by} #{to}"
end
p Counter.new.step(by: 2)
p Counter.new.step(to: 5, by: 2)
