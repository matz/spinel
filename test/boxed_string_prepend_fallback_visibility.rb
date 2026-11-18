module Outside
  def length = super + 100
end
class Friend
  def length = 40
end
def sample(v)
  p [v.respond_to?(:length), v.respond_to?(:length,true), String.public_method_defined?(:length),String.protected_method_defined?(:length),String.private_method_defined?(:length)]
  begin
    p [:ordinary,v.length]
  rescue NoMethodError => e
    p [:hidden,e.name,e.args]
  end
  p [:blind,v.send(:length)]
end
v=[+"abc",Friend.new][0]
class String
  prepend Outside if ENV['LAYER']
end
sample(v)
class String
  def length = 22
end
sample(v)
class String
  protected :length if (ENV.fetch("SPINEL_TEST_EARLY_PREPEND", "yes") == "yes")
end
sample(v)
class String
  private :length if (ENV.fetch("SPINEL_TEST_EARLY_PREPEND", "yes") == "yes")
end
sample(v)
class String
  public :length if (ENV.fetch("SPINEL_TEST_EARLY_PREPEND", "yes") == "yes")
end
sample(v)
