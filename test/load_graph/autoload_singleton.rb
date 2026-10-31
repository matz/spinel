class AutoSingleton
  class << self
    autoload :AutoFoo, File.join(__dir__, "autoload_singleton_lib")
    def read = AutoFoo
  end
end
p AutoSingleton.read
