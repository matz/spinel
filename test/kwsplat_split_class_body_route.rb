# A split call in a class body that runs before the class defines its own method of the name is still Kernel's.
$e = {z: 0}.reject { true }
class U
  A = rand(1, **$e)
  def self.rand(*a) = [:mine, *a]
end
p U::A
p U.rand(3, **$e)
