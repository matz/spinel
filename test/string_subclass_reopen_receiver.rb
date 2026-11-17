class String
  def read(x=self) = x
  def decorate = self << "!"
end
class One < String
  def read(x=self) = super
  def decorate = super
end
class Two < One
  def decorate = super
end
class Three < Two
  def decorate = super
end
s=Three.new("abc")
p s.read.equal?(s)
a=s.read
a << "x"
p s.decorate.equal?(s)
p s
