puts "singleton lib loaded"
class AutoSingleton
  class << self
    AutoFoo = 17
  end
end
