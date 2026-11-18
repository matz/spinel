# The root Object initializer has no user Scope; bare super gathers its rest.
class RestInitializer
  def initialize(*arguments)
    super
  end
end
p RestInitializer.new.class
begin
  RestInitializer.new(1)
rescue ArgumentError => error
  p [error.class, error.message]
end
