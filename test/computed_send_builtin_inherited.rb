# A computed send on a builtin receiver reaches what the class inherits --
# the methods every object answers, Enumerable's, Comparable's and
# Numeric's -- and what the program's reopen of the class defines. Another
# class's method of the same name and a different arity does not remove the
# builtin's arm.
class Box
  def empty?(x) = x
end
p Box.new.empty?(1)

class String
  def shout? = true
end

s = "abc"
%w[frozen empty shout].each { |q| p s.public_send("#{q}?") }
%w[itself dup].each { |q| p s.public_send("#{q}") }
a = [3, 1, 2]
%w[frozen nil].each { |q| p a.public_send("#{q}?") }
p a.public_send("#{"mi"}n")
p a.public_send("#{"each_sli"}ce", 2).to_a
i = 7
%w[positive negative].each { |q| p i.public_send("#{q}?") }
p i.public_send("#{"betw"}een?", 1, 9)
