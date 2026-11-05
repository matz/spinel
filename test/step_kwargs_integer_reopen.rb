# A reopened Integer's own step takes the keywords; Float keeps Numeric#step
class Integer
  def step(to: 0, by: 1) = "mine #{self} #{to} #{by}"
end
p 1.step(to: 3, by: 1)
p 1.step(by: 2)
p 2.5.step(to: 3.5, by: 0.5).to_a
