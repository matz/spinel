# Fancy is defined only when the environment variable is set, so CRuby
# rescues the NameError and Widget is a subclass of Base. The program text
# does not say whether the branch runs, so spinel refuses the class instead
# of picking Fancy.
class Base
end

if ENV["SPINEL_NOT_SET"]
  class Fancy < Base
  end
end

class Widget < (Fancy rescue Base)
end

puts Widget.superclass
