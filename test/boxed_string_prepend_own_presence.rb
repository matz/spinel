class Fancy < String
end
module Outside
  def length = 18
end
def ask
  p [Fancy.public_method_defined?(:length), Fancy.public_method_defined?(:length,false), Fancy.private_method_defined?(:length), Fancy.private_method_defined?(:length,false)]
end
class String
  prepend Outside if (ENV["SPINEL_TEST_EARLY_PREPEND"] == "yes")
end
ask
class String
  private
  def length = 22
end
ask
