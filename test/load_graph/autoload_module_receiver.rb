# An explicit module receiver registers the module's constant.
module AutoMod
end
AutoMod.autoload :Z, File.join(__dir__, "autoload_module_lib")
p AutoMod::Z
