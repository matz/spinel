module U
  module_function
  def a(x) = x + 1
  def b(x) = a(x) * 2
end
module V
  def a(x) = x + 1
  def b(x) = a(x) * 2
  module_function :a, :b
end
