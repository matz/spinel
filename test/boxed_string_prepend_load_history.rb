module AddTen
  def size = super + 10
end
class Other
  def size = 90
end
def read(v)
  v.size
end
v = [+"abc", Other.new][0]
p read(v)
class String
  def size = 12
end
p read(v)
class String
  prepend AddTen
end
p read(v)
class String
  def size = 20
end
p read(v)
