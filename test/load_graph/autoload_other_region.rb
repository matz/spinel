def pull(path)
  require path
end
def discover
  require_relative "autoload_register_lib"
end
pull(File.join(__dir__, "autoload_register_lib.rb"))
p AutoFoo.five
