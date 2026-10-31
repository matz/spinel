# A reference after its method's definition loads once, including in a loop.
def auto_read = AutoFoo.new.hi
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
2.times { p auto_read }
2.times { p AutoFoo.five }
