module B
  def zork(a, b = 2, k: 3) = [:b, a, b, k]
end
class C
  include B
end
