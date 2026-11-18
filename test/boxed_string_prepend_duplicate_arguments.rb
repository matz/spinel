module A
  def size = 10 * super + 1
end
module B
  def size = 10 * super + 2
end
v = +"abc"
class String
  prepend A if ENV["SPINEL_TEST_EARLY_PREPEND"] == "yes"
  prepend A, B, A
end
p v.size
