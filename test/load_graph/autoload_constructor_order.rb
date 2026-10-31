# A resolved constructor receiver loads after an earlier argument.
def auto_take(first, second)
  p first, second
end
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
auto_take([puts("earlier")], AutoFoo.new.hi)
